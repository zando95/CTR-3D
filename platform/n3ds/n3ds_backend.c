/*
 * libctru side of the backend boundary. This file is its own translation unit
 * and must never include game headers -- see n3ds_backend.h for why.
 */

#include <3ds.h>
#include <citro3d.h>
#include <malloc.h>
#include <netinet/in.h> /* full struct in_addr for __3dslink_host (3dslink.h only forward-declares it) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* fsync -- the SD log's timed durability commit */

#include "n3ds_backend.h"
#include "n3ds_beat.h"
#include "n3ds_render_thread.h" /* N3DS_RenderWorkerIsSelf -- which thread the frame-begin guard is running on */
#include "ctr_shbin.h"
/* The track and model sibling shaders, built by the exact same picasso ->
 * bin2s pipeline as ctr_shbin.h above (CMakeLists.txt) -- see
 * platform/n3ds/shaders/ctr_track.v.pica and ctr_model.v.pica. */
#include "ctr_track_shbin.h"
#include "ctr_model_shbin.h"

/*
 * Diagnostics go to two places because neither alone is sufficient:
 *   - svcOutputDebugString, which Luma surfaces on hardware, but which Azahar's
 *     release build compiles out (its LOG_DEBUG is stripped).
 *   - an absolute-path file on the SD card, which survives on both and can be
 *     read back from the host without a screenshot. The path is absolute
 *     because the working directory is not dependable this early.
 */
#define N3DS_LOG_PATH "sdmc:/ctr3ds.log"
#define N3DS_LOG_PREV_PATH "sdmc:/ctr3ds.prev.log"

/*
 * Overrides libctru's weak default. Left at 0, libctru halves the free
 * application memory between the two heaps and caps the linear half at 32 MiB:
 * a New 3DS reaches the cap, an original 3DS gets about 23 MiB, short of the
 * decode-cache atlases. Fixed here, the linear heap is the same on both and
 * the application heap takes whatever is left.
 */
u32 __ctru_linear_heap_size = 32u * 1024u * 1024u;

static FILE *s_logFile = NULL;

/*
 * Guards N3DS_DebugLog's buffered-sink append + throttled-flush block
 * (stderr and s_logFile's fputs/fputc, the shared flush-gate timestamp, and
 * the [LOGFLUSH] accumulators N3DS_LogFlushStats drains). svcOutputDebugString
 * needs no lock -- it is a syscall, not buffered stdio.
 *
 * N3DS_DebugLog has three legitimate callers -- the game thread, the render
 * worker (N3DS_GuardedFrameBegin's stall logging), and the audio thread (the
 * [BEAT] watchdog) -- so two threads can genuinely append to the SAME FILE*
 * at once. devkitARM's newlib does not lock stdio's internal buffer state
 * across threads, so an unguarded concurrent fputs/fflush pair here would
 * race on that buffer's write pointer. Initialized in N3DS_EarlyInit, before
 * any other thread exists.
 */
static LightLock s_logBufferLock;

/*
 * 3dslink netload stderr streaming: when sent over Wi-Fi via 3dslink, HBL
 * records the sender's IP in __3dslink_host; connect a TCP stream back to it
 * (3dslink must run with -s) and point stderr at it so every log line
 * mirrors to the PC live. stdout stays the bottom-screen console; SD-card /
 * Azahar launches have s_addr == 0 and skip this.
 */
#define N3DS_SOC_ALIGN       0x1000
#define N3DS_SOC_BUFFER_SIZE 0x100000

static u32 *s_socBuffer = NULL;
static int s_netloadStreamActive = 0;

int N3DS_NetloadStreamActive(void)
{
	return s_netloadStreamActive;
}

int N3DS_SdLogEnabled(void)
{
	extern int g_dbg_sdLog;

	return g_dbg_sdLog;
}

void N3DS_SetSdLogEnabled(int enabled)
{
	extern int g_dbg_sdLog;

	g_dbg_sdLog = (enabled != 0);
}

static void N3DS_NetloadStreamExit(void)
{
	if (s_socBuffer != NULL)
	{
		socExit();
	}
}

static void N3DS_NetloadStreamInit(void)
{
	if (__3dslink_host.s_addr == 0)
	{
		return;
	}

	s_socBuffer = (u32 *)memalign(N3DS_SOC_ALIGN, N3DS_SOC_BUFFER_SIZE);
	if (s_socBuffer == NULL)
	{
		return;
	}

	if (R_FAILED(socInit(s_socBuffer, N3DS_SOC_BUFFER_SIZE)))
	{
		free(s_socBuffer);
		s_socBuffer = NULL;
		return;
	}

	atexit(N3DS_NetloadStreamExit);

	/* stderr only: stdout must stay the bottom-screen console. If the host
	 * isn't listening (3dslink without -s) this fails fast and boot proceeds
	 * with the socket service up but no stream -- harmless. */
	if (link3dsConnectToHost(false, true) < 0)
	{
		return;
	}

	/* Batch the stream: newlib's stderr is unbuffered, so every log line is a
	 * synchronous soc:u send, and each send is a chance for a transient WiFi
	 * failure to kill the whole stream. Buffer + the shared 2s timed flush in
	 * N3DS_DebugLog turns that into one send per window batch; must precede
	 * the first stderr write, hence before the active flag flips (every
	 * stream write is gated on it). svcOutputDebugString stays the per-line
	 * instant trail for crash forensics. */
	setvbuf(stderr, NULL, _IOFBF, 8192);

	s_netloadStreamActive = 1;
}

/* One failed open, not one per line: an unwritable card would otherwise cost
 * an SD round trip for every message. Matches the single attempt the eager
 * open used to make. */
static int s_logOpenFailed = 0;

/*
 * Opens the SD log on the first message that is actually going to be written,
 * so a build with logging off leaves no ctr3ds.log on the card at all. Callers
 * must hold s_logBufferLock: every other s_logFile touch in this file does,
 * and two threads log concurrently.
 */
static void N3DS_LogOpenFileLocked(void)
{
	if ((s_logFile != NULL) || s_logOpenFailed)
	{
		return;
	}

	/* One-deep rotation BEFORE the truncating open: "w" wipes the previous
	 * run's log, so a crash -> relaunch sequence would otherwise truncate
	 * the crashed run's log before it can be read. Keeps exactly the last
	 * two, never grows unbounded. */
	remove(N3DS_LOG_PREV_PATH);
	rename(N3DS_LOG_PATH, N3DS_LOG_PREV_PATH);

	s_logFile = fopen(N3DS_LOG_PATH, "w");
	if (s_logFile != NULL)
	{
		/* 16KB stdio buffer: at the stats block's ~2-3KB/s the default ~1KB
		 * buffer would auto-flush (an SD stall) mid-window anyway, defeating
		 * the timed flush in N3DS_DebugLog. */
		setvbuf(s_logFile, NULL, _IOFBF, 16384);
	}
	else
	{
		s_logOpenFailed = 1;
	}
}

void N3DS_EarlyInit(void)
{
	/* Before anything can call N3DS_DebugLog (the netload-stream connect
	 * message a few lines down does exactly that) and long before any worker
	 * thread exists to contend for it. */
	LightLock_Init(&s_logBufferLock);

	consoleDebugInit(debugDevice_SVC);

	N3DS_NetloadStreamInit();
	if (s_netloadStreamActive)
	{
		N3DS_DebugLog("[CTR-3DS] netload stderr stream connected");
	}
}

/* Last SD-log flush, in system ticks. Per-LINE fflush stalls gameplay
 * visibly on hardware: each flush pays FAT + card latency (tens of ms), and
 * the stats window emits ~15 lines back-to-back, so every window freezes the
 * frame for a beat. Invisible in Azahar, whose sdmc is a host folder with
 * free flushes. */
static u64 s_logLastFlushTick = 0;

/*
 * SD-card logging master switch, default off: the 3DS SD stack is slow at
 * file work, and N3DS_DebugLog's throttled fflush+fsync runs on the game
 * thread every ~2s, costing one stalled frame per window -- enough to erase
 * the margin to a solid 30fps.
 *
 * Off still leaves svcOutputDebugString per line (free with no debugger
 * attached, and Azahar still captures it) and the whole bottom-screen
 * readout, which is CPU-only. Turn back on when a log is actually wanted --
 * SELECT+R on hardware, dumpreq `sdlog`.
 */
int g_dbg_sdLog = 0;

/* [LOGFLUSH] accumulators -- see the timing block inside N3DS_DebugLog. */
static unsigned s_logFlushWorstUs = 0;
static unsigned s_logFlushTotalUs = 0;
static unsigned s_logFlushCount = 0;

void N3DS_DebugLog(const char *msg)
{
	if (msg == NULL)
	{
		return;
	}

	svcOutputDebugString(msg, (int)strlen(msg));

	/* The render worker logs from the OT walk; if it is ever seen parked
	 * here by the watchdog, the SD flush on another thread is what the
	 * game thread is really waiting for. */
	if (N3DS_RenderWorkerIsSelf())
	{
		N3DS_BeatMark(N3DS_BEAT_THREAD_RENDERWORKER, N3DS_BEAT_WHERE_LOGWAIT);
	}
	LightLock_Lock(&s_logBufferLock);
	if (N3DS_RenderWorkerIsSelf())
	{
		N3DS_BeatMark(N3DS_BEAT_THREAD_RENDERWORKER, N3DS_BEAT_WHERE_OK);
	}

	if (s_netloadStreamActive)
	{
		fputs(msg, stderr);
		fputc('\n', stderr);
	}

	/* g_dbg_sdLog off: no fputs either, not just no flush, and no file. A
	 * buffered write is only free until the 16KB buffer fills, and then stdio
	 * flushes it wherever it happens to be -- mid-corner, unthrottled, exactly
	 * the stall this switch exists to remove. */
	if (g_dbg_sdLog)
	{
		N3DS_LogOpenFileLocked();
	}

	if ((s_logFile != NULL) && g_dbg_sdLog)
	{
		fputs(msg, s_logFile);
		fputc('\n', s_logFile);
	}

	/* Flush BOTH buffered sinks at most every ~2s (SD file and the netload
	 * stderr stream -- see the setvbuf in N3DS_NetloadStreamInit). Both are
	 * post-hoc trails that tolerate a bounded tail loss on a hard crash;
	 * svcOutputDebugString above is the per-line instant trail. */
	if (((s_logFile != NULL) && g_dbg_sdLog) || s_netloadStreamActive)
	{
		const u64 now = svcGetSystemTick();

		if ((now - s_logLastFlushTick) > (SYSCLOCK_ARM11 * 2ull))
		{
			/* Time the flush itself. Every 64-frame window in the Tubes log
			 * carries exactly ONE frame of 76-176ms, and this 2s-throttled SD
			 * write is the only thing in the build whose cadence matches. One
			 * hitch per window costs ~1.8ms of the window AVERAGE, which is
			 * the whole remaining gap to a solid 30 -- so if this is it,
			 * every perf number we have is carrying our own instrument on
			 * its back. Reported as [LOGFLUSH]. */
			const u64 flushStart = svcGetSystemTick();

			if ((s_logFile != NULL) && g_dbg_sdLog)
			{
				fflush(s_logFile);
				/* fflush alone moves bytes into the FS session; the FAT
				 * size/allocation metadata only commits on an fs-level
				 * flush. Without fsync, a hard freeze loses everything
				 * written after the first flush, since the file's
				 * metadata never committed. */
				fsync(fileno(s_logFile));
			}
			if (s_netloadStreamActive)
			{
				fflush(stderr);
			}
			s_logLastFlushTick = now;

			{
				const u64 took = svcGetSystemTick() - flushStart;
				const unsigned tookUs = (unsigned)((took * 1000000ull) / (u64)SYSCLOCK_ARM11);

				s_logFlushTotalUs += tookUs;
				if (tookUs > s_logFlushWorstUs)
				{
					s_logFlushWorstUs = tookUs;
				}
				s_logFlushCount++;
			}
		}
	}

	LightLock_Unlock(&s_logBufferLock);
}

/* Immediate SD commit of everything buffered so far, bypassing the timed
 * flush gate: used the instant a freeze/thaw is detected, because the
 * evidence that the process is still alive afterwards is worthless if it
 * only ever sits in a buffer the next power-cycle discards. */
void N3DS_LogFlushNow(void)
{
	LightLock_Lock(&s_logBufferLock);
	if (s_logFile != NULL)
	{
		fflush(s_logFile);
		fsync(fileno(s_logFile));
	}
	LightLock_Unlock(&s_logBufferLock);
}

void N3DS_LogFlushStats(unsigned *worstUs, unsigned *totalUs, unsigned *count)
{
	LightLock_Lock(&s_logBufferLock);
	*worstUs = s_logFlushWorstUs;
	*totalUs = s_logFlushTotalUs;
	*count = s_logFlushCount;
	s_logFlushWorstUs = 0;
	s_logFlushTotalUs = 0;
	s_logFlushCount = 0;
	LightLock_Unlock(&s_logBufferLock);
}

/*
 * VRAM debug capture (CTR_INTERNAL, texture-corruption diagnostic; part of
 * the dumpreq `capture` set). The renderer TU owns vram[] but the trigger
 * plumbing lives in this TU, which can't see vram[]. So the renderer
 * registers the buffer pointer once and the trigger + fwrite happen here.
 */
static const void *s_dumpVramPtr = NULL;
static unsigned int s_dumpVramBytes = 0;
static const unsigned int *s_dumpFrameCounterPtr = NULL;

void N3DS_DebugSetVRAMForDump(const void *vram, unsigned int bytes, const unsigned int *frameCounter)
{
	s_dumpVramPtr = vram;
	s_dumpVramBytes = bytes;
	s_dumpFrameCounterPtr = frameCounter;
}

#ifdef CTR_INTERNAL
/* Frame number stamped into every capture filename. Fixed names already cost
 * one race capture (overwritten by a later menu capture); the
 * shared stamp also groups the vramdump/atlas/cachemeta files of one capture
 * instant into a self-identifying set. */
static unsigned int N3DS_DebugCaptureFrame(void)
{
	return (s_dumpFrameCounterPtr != NULL) ? *s_dumpFrameCounterPtr : 0u;
}
#endif

#ifdef CTR_INTERNAL
static void N3DS_DebugWriteVRAMDump(void)
{
	FILE *f;
	char path[64];
	char msg[96];

	if ((s_dumpVramPtr == NULL) || (s_dumpVramBytes == 0))
	{
		N3DS_DebugLog("[CTR-3DS] VRAM dump skipped: buffer not registered");
		return;
	}

	snprintf(path, sizeof(path), "sdmc:/vramdump_%u.bin", N3DS_DebugCaptureFrame());
	f = fopen(path, "wb");
	if (f == NULL)
	{
		snprintf(msg, sizeof(msg), "[CTR-3DS] VRAM dump FAILED: fopen %s", path);
		N3DS_DebugLog(msg);
		return;
	}

	fwrite(s_dumpVramPtr, 1, s_dumpVramBytes, f);
	fclose(f);

	snprintf(msg, sizeof(msg), "[CTR-3DS] VRAM dumped to %s (%u bytes)", path, s_dumpVramBytes);
	N3DS_DebugLog(msg);
}
#endif

unsigned long long N3DS_GetTickCount(void)
{
	return (unsigned long long)svcGetSystemTick();
}

unsigned long long N3DS_GetTickFrequency(void)
{
	return (unsigned long long)SYSCLOCK_ARM11;
}

void N3DS_SleepMs(unsigned int ms)
{
	svcSleepThread((s64)ms * 1000000LL);
}

/* 60 seconds of vblanks. The panel is the last thing the process does, so a
 * console left on a shelf still powers down rather than sitting lit. */
#define N3DS_FATAL_SCREEN_VBLANKS 3600

/* Whether gfxInitDefault (N3DS_GfxInit) and C3D_Init (N3DS_C3DInit) are
 * currently in force. The fatal screen can be reached after either succeeded
 * and a later step failed, and must release them before taking the screen. */
static int s_n3dsGfxActive;
static int s_n3dsC3DActive;

int N3DS_IsNew3DS(void)
{
	bool isNew3DS = false;

	return R_SUCCEEDED(APT_CheckNew3DS(&isNew3DS)) && isNew3DS;
}

void N3DS_ShowFatalScreen(const char *message)
{
	unsigned int vblanks = 0;
	const char *line;

	/*
	 * The failure text also went to stderr, which N3DS_EarlyInit pointed at
	 * svcOutputDebugString -- invisible on a console with no debugger
	 * attached, which is why the panel exists at all.
	 */
	if (s_n3dsC3DActive)
	{
		C3D_Fini();
		s_n3dsC3DActive = 0;
	}
	if (s_n3dsGfxActive)
	{
		gfxExit();
		s_n3dsGfxActive = 0;
	}

	gfxInitDefault();
	consoleInit(GFX_TOP, NULL);

	printf("\n  CTR 3D\n\n");
	for (line = (message != NULL) ? message : ""; *line != '\0';)
	{
		const char *end = strchr(line, '\n');
		int len = (end != NULL) ? (int)(end - line) : (int)strlen(line);

		printf("  %.*s\n", len, line);
		line += len;
		if (*line == '\n')
		{
			line++;
		}
	}
	printf("\n  Press any button to exit.\n");

	while (aptMainLoop() && (vblanks < N3DS_FATAL_SCREEN_VBLANKS))
	{
		hidScanInput();
		if (hidKeysDown() != 0)
		{
			break;
		}

		gfxFlushBuffers();
		gfxSwapBuffers();
		gspWaitForVBlank();
		vblanks++;
	}

	gfxExit();
}

void N3DS_GfxInit(void)
{
	/*
	 * New3DS 804MHz clock + the extra L2 cache. Nothing in this build asked
	 * for them before: the only osSetSpeedupEnable in the tree is inside
	 * SDL's 3DS main shim, and the 3DS build does not link SDL at all. The
	 * owner's CFW (Luma/Rosalina) forces both on regardless, so this is not
	 * expected to move the frame rate on THIS console -- it is here so the
	 * build does not silently depend on someone else's CFW configuration.
	 * Harmless on an Old3DS, where libctru makes it a no-op.
	 */
	osSetSpeedupEnable(true);

	gfxInitDefault();
	s_n3dsGfxActive = 1;

	/*
	 * Stereoscopic 3D: double-buffer the top screen's RIGHT framebuffer too.
	 * Costs ~576KB of linear heap up front; with the physical slider at zero
	 * the parallax barrier is off, the right buffer is never presented, and
	 * the render path draws one eye exactly as before (N3DS_StereoLatchFrame).
	 * Called here, before any framebuffer is fetched, because gfxSet3D
	 * reallocates the top screen's buffers.
	 */
	gfxSet3D(true);

	/*
	 * The debug/stats console lives on the bottom screen now that citro3d
	 * (N3DS_C3DInit, below) owns the top screen's render target. The two
	 * coexist because they drive different physical screens through
	 * different presentation paths -- see N3DS_GfxPresent.
	 */
	consoleInit(GFX_BOTTOM, NULL);
}

void N3DS_GfxExit(void)
{
	if (s_n3dsGfxActive)
	{
		gfxExit();
		s_n3dsGfxActive = 0;
	}
}

/*
 * Present flushes only the BOTTOM framebuffer, not the whole-system
 * gfxFlushBuffers().
 *
 * gfxFlushBuffers cleans the data cache for top-left, top-right AND bottom
 * (~806KB/frame with gfxSet3D on) via svcFlushProcessDataCache. libctru's own
 * header says it "is only used during software rendering" and "has
 * significant overhead" -- and our only software rendering is the bottom
 * console, which redraws once every 64 frames. The two top framebuffers are
 * written by the GPU (C3D_FrameEnd's display transfer), never by the CPU, so
 * cleaning their cache lines is ~576KB of pure waste every frame.
 *
 * The one thing that ever READS them on the CPU is the pause freeze-frame
 * (N3DS_CopyDisplayedTopFrame); it does its own invalidate, which is where
 * that belongs. Hardware-measured: present 3.70 -> 1.67ms.
 */

/* Bottom-screen minimap present-side TTL -- declared up here because
 * N3DS_GfxPresent consumes it; set by N3DS_BottomMapDraw_Begin (much further
 * down, with the rest of that pass's state). */
static int s_bottomMapOwnFrames = 0;
/* Armed at the first present: from then on the console only paints when the
 * stats toggle asks for it (release posture -- the bottom panel stays black
 * outside the map). Anything printed BEFORE the first present (boot banners,
 * an asset error that aborts before the game runs) stays on screen. */
static int s_consoleQuietArmed = 0;

/*
 * Single console-visibility answer for every glyph painter: the stats drip
 * (native_renderer_3ds.c) and Platform_Log's stdout echo (native_log.c).
 * Glyphs land in the shared bottom framebuffer at PRINT time, so visibility
 * must be enforced where text is painted, not at present/flush time.
 */
int N3DS_ConsolePaintAllowed(void)
{
#ifdef CTR_INTERNAL
	extern int g_dbg_statsConsole; /* renderer TU, dumpreq 'stats' */

	if (s_bottomMapOwnFrames > 0)
	{
		return 0; /* the map owns the panel */
	}
	if (g_dbg_statsConsole)
	{
		return 1; /* dev sitting */
	}
	return !s_consoleQuietArmed; /* boot text until the game presents */
#else
	/* Release: log text is developer output on a player's screen. It still
	 * reaches the log file and the netload mirror; the panel carries only the
	 * game's own surfaces (the map, and a pre-game hold's status row). */
	return 0;
#endif
}

/* 1-based console row this line owns; 0 until the first paint claims one. */
static int s_consoleStatusRow = 0;
/* Columns the row is painted across. Clamped so the format below cannot be
 * handed an absurd width by a console whose metrics did not initialise. */
#define N3DS_CONSOLE_STATUS_MAX_COLS 64

/* Claims the first free row under whatever text is already on the panel, so a
 * status line lands below the boot output rather than over it. */
static void N3DS_ConsoleStatusClaimRow(const PrintConsole *con)
{
	int rows = ((con != NULL) && (con->consoleHeight > 0)) ? con->consoleHeight : 30;

	s_consoleStatusRow = ((con != NULL) ? con->cursorY : 0) + 1;
	if (s_consoleStatusRow < 1)
	{
		s_consoleStatusRow = 1;
	}
	if (s_consoleStatusRow > rows)
	{
		s_consoleStatusRow = rows;
	}
}

/*
 * Repaints one console row in place: absolute cursor position to column 1,
 * then the text padded out to the console's full width. The padding is what
 * makes the row OWNED -- an overwrite only replaces the cells it prints, so a
 * shorter update would otherwise leave a longer predecessor's tail standing to
 * its right. No consoleClear and no newline: a full repaint erases all 40x30
 * cells before drawing any text and cost ~50ms a window when the stats block
 * did it (these glyph cells go into a column-major rotated framebuffer), and a
 * newline would scroll the text above this row away.
 *
 * One call is one row of cells, so the caller owns the update rate.
 *
 * Not routed through N3DS_ConsolePaintAllowed: that gate keeps log text off a
 * player's panel, and reserves it for the game's own surfaces, which a
 * pre-game hold's status row is one of. The map TTL still applies, since that
 * pass shares the framebuffer.
 */
void N3DS_ConsoleStatusLine(const char *text)
{
	const PrintConsole *con = consoleGetDefault();
	int width;

	if (s_bottomMapOwnFrames > 0)
	{
		return;
	}
	if (s_consoleStatusRow == 0)
	{
		N3DS_ConsoleStatusClaimRow(con);
	}

	width = ((con != NULL) && (con->consoleWidth > 0)) ? con->consoleWidth : 40;
	if (width > N3DS_CONSOLE_STATUS_MAX_COLS)
	{
		width = N3DS_CONSOLE_STATUS_MAX_COLS;
	}
	printf("\x1b[%d;1H%-*.*s", s_consoleStatusRow, width, width, (text != NULL) ? text : "");
}

/* Blanks the status row and releases it, so the line does not survive into the
 * game and a later hold claims its own row. */
void N3DS_ConsoleStatusClear(void)
{
	if ((s_consoleStatusRow != 0) && (s_bottomMapOwnFrames == 0))
	{
		N3DS_ConsoleStatusLine("");
	}
	s_consoleStatusRow = 0;
}

void N3DS_GfxPresent(void)
{
	/*
	 * Bottom-screen minimap owns the panel: C3D_FrameEnd's display transfer is
	 * presenting the bottom screen on those frames, and the console's text
	 * shares the same single-buffered framebuffer -- flushing/swapping for the
	 * console here would scribble stale text over the map. The TTL (set by
	 * N3DS_BottomMapDraw_Begin) expires within a few presents of the map pass
	 * going quiet, and the console resumes untouched.
	 */
	if (!s_consoleQuietArmed)
	{
		/* First present: the game is running, so wipe the boot text and go
		 * quiet (see the flag's comment). One-shot. */
		s_consoleQuietArmed = 1;
		consoleClear();
	}
	if (s_bottomMapOwnFrames > 0)
	{
		s_bottomMapOwnFrames--;
		if (s_bottomMapOwnFrames == 0)
		{
			/* The map just released the panel and (with the stats console off)
			 * nothing else will ever repaint it -- wipe the dead map frame so
			 * the bottom screen returns to black rather than freezing. With
			 * stats ON the drip repaints its rows over the cleared screen. */
			consoleClear();
		}
		return;
	}

	/*
	 * Only the bottom screen's console framebuffer goes through this path.
	 * The generic gfxSwapBuffers() swaps every screen's buffers, which would
	 * fight citro3d for ownership of the top screen -- C3D_FrameEnd (see
	 * N3DS_C3DEndFrame) already presents that one.
	 */
	{
		u16 fbW = 0, fbH = 0;
		u8 *fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbW, &fbH);

		/* The buffer this returns is the one the console just drew into and
		 * the one the swap below is about to present -- flush exactly it.
		 * Unconditional rather than dirty-tracked: console writes come from
		 * ~90 printf sites (boot banners, the 64-frame stats block, the atlas
		 * inspector), and a missed flush would silently blank text that is
		 * the only readout hardware has. Bottom-only is already the bulk of
		 * the win; skipping clean frames too can come later, with a hook that
		 * can't miss a writer. */
		if (fb != NULL)
		{
			GSPGPU_FlushDataCache(fb, (u32)fbW * (u32)fbH * 3U);
		}
	}

	gfxScreenSwapBuffers(GFX_BOTTOM, false);
}

/*
 * Copies the top screen's last completed gfx framebuffer into dstRGB as
 * row-major RGB triplets, nearest-resampled from the pillarboxed CONTENT
 * region (N3DS_TOP_CONTENT_*, header) to dstW x dstH.
 * The source costs nothing
 * to produce: citro3d's C3D_FrameEnd already display-transfers every frame
 * into the linear-heap gfx framebuffers (GSP_BGR8_OES via gfxInitDefault;
 * the transfer's OUT_FORMAT is RGB8, which is what fills that byte layout),
 * so this is a plain CPU read of memory the present path maintains anyway --
 * no GX call, no GPU sync.
 *
 * gfxGetFramebuffer returns the BACK buffer (the one drawn into next), whose
 * content is the frame presented one swap before the current front -- so the
 * copy can be one presentation old. Acceptable by design for the freeze-
 * frame use: the consumer is a paused/transitioning scene.
 *
 * Rotation: the 3DS top framebuffer is stored rotated -- fbW(=240) is the
 * panel's SHORT axis and consecutive bytes walk UP a screen column, so the
 * pixel at screen (sx, sy), y-down, lives at ((sx * 240) + (239 - sy)) * 3,
 * bytes B,G,R.
 */
/*
 * REAL-widescreen content fills the whole 400px panel; the 4:3 modes render
 * into the pillarboxed N3DS_TOP_CONTENT region. The copy below must sample
 * whichever region the game actually drew, or wide-mode consumers (pause
 * background, fb-capture) re-ingest a crop of the picture and the draw side
 * stretches it back out -- the reported crop-then-stretch pause background.
 * Published by the renderer (NativeRenderer_StoreFrameBuffer, every
 * EndScene); a plain int across the TU boundary like every other flag here.
 */
static int s_topContentFullWidth = 0;

void N3DS_SetTopContentFullWidth(int fullWidth)
{
	s_topContentFullWidth = fullWidth;
}

int N3DS_CopyDisplayedTopFrame(unsigned char *dstRGB, int dstW, int dstH)
{
	u16 fbW = 0, fbH = 0;
	u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
	const int contentX = s_topContentFullWidth ? 0 : N3DS_TOP_CONTENT_X;
	const int contentW = s_topContentFullWidth ? 400 : N3DS_TOP_CONTENT_W;
	int x, y;

	if ((fb == NULL) || (dstRGB == NULL) || (dstW <= 0) || (dstH <= 0))
	{
		return 0;
	}

	/* This buffer is written by the GPU (C3D_FrameEnd's display transfer) and
	 * read here by the CPU, so the CPU's cached view of it may be stale. This
	 * used to be covered by accident -- N3DS_GfxPresent's whole-system
	 * gfxFlushBuffers cleaned and invalidated all three framebuffers every
	 * frame. The bottom-only present stops paying that 3.3ms, so the one reader that
	 * actually needed it asks for it directly, on pause entry rather than
	 * 30 times a second. */
	GSPGPU_InvalidateDataCache(fb, (u32)fbW * (u32)fbH * 3U);

	for (y = 0; y < dstH; y++)
	{
		const int sy = (y * 240) / dstH;
		unsigned char *dstRow = dstRGB + ((size_t)y * (size_t)dstW * 3);

		for (x = 0; x < dstW; x++)
		{
			/* Sample only the region the game drew (see s_topContentFullWidth
			 * above): the pillarboxed content region in 4:3 modes -- the
			 * full-panel form re-ingested the black side bars, so the pause
			 * background showed them squeezed inside the picture
			 * (user-reported on hardware) -- or the whole panel in REAL wide. */
			const int sx = contentX + ((x * contentW) / dstW);
			const u8 *p = fb + (((sx * 240) + (239 - sy)) * 3);

			dstRow[(x * 3) + 0] = p[2]; /* R */
			dstRow[(x * 3) + 1] = p[1]; /* G */
			dstRow[(x * 3) + 2] = p[0]; /* B */
		}
	}

	return 1;
}

/* ------------------------------------------------------------- render --- */

#define N3DS_DISPLAY_TRANSFER_FLAGS                                                                         \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |                        \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |          \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static DVLB_s *s_shaderDvlb = NULL;
static shaderProgram_s s_shaderProgram;
static int s_uLocProjection = -1;
static int s_uLocStereo = -1;
static int s_uLocStereoUi = -1;
/*
 * UI pop-out amount, as a percent of this eye's at-infinity shift k --
 * explicitly not at the screen plane, and less than the character-select
 * rotating character.
 *
 * A percent of k rather than an absolute pixel count tracks the 3D slider for
 * free and is identically 0 when the slider is down -- mono safety by
 * construction, the same property the flag depth/parallax split relies on.
 *
 * Runtime-tunable (dumpreq 'uipop N') because this is a comfort value judged
 * by eye, on hardware.
 */
int g_dbg_uiPopPct = 35;
static float s_stereoVecUi[2][4];

/* Mirror mode: the world is drawn mirrored this frame (N3DS_SetMirrorWorld), about the packet
 * window's centre. s_mirrorAxisSum is left + right of that window, so x' = s_mirrorAxisSum - x. */
static int s_mirrorWorld = 0;
static float s_mirrorAxisSum = 512.0f;
static C3D_RenderTarget *s_renderTarget = NULL;

static C3D_Mtx s_projection;
static void *s_vertexBufferMem = NULL;
static int s_vertexCursor = 0;
static int s_vertexHighWater = 0;
static C3D_Tex s_atlasTextures[N3DS_ATLAS_COUNT];
static C3D_Tex s_vramDisplayTex[N3DS_VRAM_DISPLAY_TEX_COUNT];

/* Native track renderer's dedicated atlas pool (rung 2b) -- see the header
 * comment on N3DS_TrackAtlas_Alloc. N3DS_TRACK_ATLAS_DIM/MAX_UNITS live in
 * n3ds_backend.h, not here -- native_track_mesh.c's
 * packer checks its own constants against those exact macros at compile
 * time. Sized to the pool's own max unit count, NOT allocated up front;
 * s_trackAtlasUnitsAllocated tracks how many of these are actually live
 * textures right now (0 until the first level load calls
 * N3DS_TrackAtlas_Alloc). */
static C3D_Tex s_trackAtlasTextures[N3DS_TRACK_ATLAS_MAX_UNITS];
static int s_trackAtlasUnitsAllocated = 0;

/* Nearest-filtered VIEWS of the same texels, for the bilinear cutout split
 * (smooth colour + crisp alpha, docs approach #2). Each is a struct copy of
 * the matching s_trackAtlasTextures[i] -- SAME .data pointer, so it shares the
 * uploaded texels and costs no extra memory -- with the filter forced to
 * GPU_NEAREST. Bound to texture unit 1 alongside the (bilinear) unit 0 so the
 * combiner can take RGB from the smooth sample and ALPHA from the sharp one,
 * giving a clean cutout with no bilinear rim. Re-copied whenever the pool is
 * (re)allocated, since a level reload changes the underlying .data. */
static C3D_Tex s_trackAtlasTexturesNearest[N3DS_TRACK_ATLAS_MAX_UNITS];

/*
 * Rung 2 model program. Only the program
 * and its uniform locations live here -- no attribute layout and no buffer
 * info, because model draws deliberately reuse the packet path's. See
 * n3ds_backend.h's N3DS_ModelDraw_* comment for why that is safe and what it
 * costs in field reinterpretation.
 */
static DVLB_s *s_modelShaderDvlb = NULL;
static shaderProgram_s s_modelShaderProgram;
static int s_modelULocQx = -1, s_modelULocQy = -1, s_modelULocQw = -1;
static int s_modelULocProjection = -1, s_modelULocH = -1, s_modelULocStereo = -1;
/* Texenv memo, same idea and lifetime rule as s_trackTexturedState: reset by
 * N3DS_ModelDraw_Begin because the packet path owns texenv 0 between model
 * frames. -1 = unknown, so the first Run after a Begin always emits. */
static int s_modelTexturedState = -1;
/* Last atlas a model Run bound. Only trusted between Begin and End -- the
 * packet path binds atlases too -- so Begin resets it. */
static int s_modelBoundAtlas = -1;
/* Depth-write memo for the model blend pass, same contract as the track's
 * s_trackDepthWriteState: -1 = not asserted in this bracket. */
static int s_modelDepthWriteState = -1;
static int s_modelDrawActive = 0;
/* Set by N3DS_ModelDraw_InstanceSkyDome until the next instance or the bracket
 * end: the sky dome's runs never write depth (see ctr_model.v.pica's skyConst). */
static int s_modelSkyDome = 0;

/*
 * Rung 3a step 2: the resident-mesh draw path's own program/attribute
 * layout/vertex+index buffer -- a second, independent set of the SAME kind
 * of state N3DS_C3DInit builds for the packet path above, since track draws
 * use a completely different vertex format (model-space N3DS_TrackVertex,
 * not screen-space N3DS_Vertex) and shader (ctr_track.v.pica's perspective
 * projection, not ctr.v.pica's orthographic passthrough).
 */
static DVLB_s *s_trackShaderDvlb = NULL;
static shaderProgram_s s_trackShaderProgram;
static int s_trackULocQx = -1, s_trackULocQy = -1, s_trackULocQw = -1;
static int s_trackULocProjection = -1, s_trackULocH = -1, s_trackULocDepthRange = -1;

/*
 * The track's drawOrder tie-break weight, in view.z units per drawOrder step,
 * uploaded as hParam.w.
 *
 * 1.0 is the shipped scale. Raising it globally cannot separate a coplanar
 * overlay pair whose authored draw_order_high bytes are EQUAL (the weight only
 * multiplies the authored gap) and shoves every other face harder in the
 * bargain -- the missing gap is baked instead (native_track_mesh.c pulls
 * turbo-pad faces 8 drawOrder steps nearer). That baked delta scales by THIS
 * value, so it must stay >= 1 to keep outranking the base quad's centre-vertex
 * relief (~5 world units).
 *
 * The shove is per-face and global -- weight * that face's signed drawOrder
 * byte -- so the ceiling below, not this number, is what keeps an extreme
 * drawOrder from pulling a road face in front of the karts standing on it.
 */
#define N3DS_TRACK_DRAWORDER_WEIGHT 1.0f
static int s_trackULocUvOffset = -1, s_trackULocStereo = -1;
/* Memo for the blend pass's depth-write state, so a frame with no semi-
 * transparent content pays nothing and a frame with some flips the pair exactly
 * twice. -1 = unknown/not set this bracket, forcing the first draw to assert. */
static int s_trackDepthWriteState = -1;
/* Rung 3b texenv memo -- see N3DS_TrackDraw_SetTextured. Declared up here with
 * the other track statics because N3DS_TrackDraw_Begin resets it and sits above
 * the function that uses it. */
static int s_trackTexturedState = -1;
static C3D_AttrInfo s_trackAttrInfo;
static C3D_BufInfo s_trackBufInfo;

/* One linearAlloc block per level (N3DS_TrackGeom_Upload's header comment):
 * verts at offset 0, indices at an 8-byte-aligned offset after them.
 * s_trackGeomReady is false until a successful upload, and N3DS_TrackDraw_Begin
 * checks it so a level whose upload failed (or hasn't happened yet, e.g. the
 * toggle flipped on before any level finished loading) draws nothing instead
 * of binding a stale/absent buffer. */
static void *s_trackGeomMem = NULL;
static const void *s_trackIndexPtr = NULL;
static int s_trackGeomReady = 0;
/* Recorded by the upload so N3DS_TrackGeom_PatchUV can bound-check a vertex
 * index against the block it is actually writing into, rather than against a
 * count the caller believes is current. The staging array the upload copied
 * from is freed immediately, so this block is the ONLY surviving copy of the
 * resident geometry -- a bad index here corrupts the level's mesh with nothing
 * to restore it from short of a full rebuild. */
static int s_trackGeomVertCount = 0;
static int s_trackGeomVertStride = 0;

/* Deferred flush range for the per-frame animated-vertex patch. The patch
 * touches thousands of scattered vertices; flushing each chunk's own
 * min..max span re-flushed nearly the whole vertex block a dozen-plus times a
 * frame (the animated verts scatter across the buffer, so every chunk's span
 * covers most of it). The patch functions now only WIDEN this byte range;
 * N3DS_TrackGeom_FlushPatched issues one GSPGPU_FlushDataCache over the union
 * at the end of the frame's patch. Bytes, into s_trackGeomMem. */
static size_t s_trackPatchLoByte = 0;
static size_t s_trackPatchHiByte = 0;
static int s_trackPatchTouched = 0;

static void N3DS_TrackGeom_MarkPatched(int lo, int hi)
{
	const size_t first = (size_t)lo * (size_t)s_trackGeomVertStride;
	const size_t last = ((size_t)hi + 1u) * (size_t)s_trackGeomVertStride;

	if (!s_trackPatchTouched)
	{
		s_trackPatchLoByte = first;
		s_trackPatchHiByte = last;
		s_trackPatchTouched = 1;
		return;
	}

	if (first < s_trackPatchLoByte)
	{
		s_trackPatchLoByte = first;
	}
	if (last > s_trackPatchHiByte)
	{
		s_trackPatchHiByte = last;
	}
}

void N3DS_TrackGeom_FlushPatched(void)
{
	if (!s_trackGeomReady || (s_trackGeomMem == NULL) || !s_trackPatchTouched)
	{
		return;
	}

	GSPGPU_FlushDataCache((u8 *)s_trackGeomMem + s_trackPatchLoByte, (u32)(s_trackPatchHiByte - s_trackPatchLoByte));
	s_trackPatchTouched = 0;
}

void *N3DS_TrackGeom_PatchBegin(int *vertStride, int *vertCount)
{
	if (!s_trackGeomReady || (s_trackGeomMem == NULL))
	{
		return NULL;
	}

	*vertStride = s_trackGeomVertStride;
	*vertCount = s_trackGeomVertCount;
	return s_trackGeomMem;
}

void N3DS_TrackGeom_PatchEnd(int loVert, int hiVert)
{
	if (s_trackGeomReady && (loVert >= 0) && (hiVert >= loVert) && (hiVert < s_trackGeomVertCount))
	{
		N3DS_TrackGeom_MarkPatched(loVert, hiVert);
	}
}

/* Set by Begin only when geometry is actually ready; End always clears it
 * and always restores packet state regardless (cheap, and foolproof against
 * any Begin/Leaf/End call-order mistake upstream). */
static int s_trackDrawActive = 0;

/* ------------------------------------------------------------- stereo 3D --- */
/*
 * See n3ds_backend.h's stereo block for the design. State here, in the order
 * it is used: master toggle and tuning (dumpreq `stereo`/`stereosep`/
 * `stereoconv`/`stereopop`), the per-frame latch, the per-eye parallax
 * vectors, and the draw journal the right eye replays.
 *
 * The parallax model is p = k * (1 - convergence/depth) in PSX screen pixels,
 * on the shared view.z axis all three geometry sources sit on. Content AT the
 * convergence depth has zero disparity (screen plane), farther
 * content goes into the screen (up to k px per eye at infinity), nearer
 * content pops out (clamped, see N3DS_StereoEyeVec). The physical slider
 * scales k continuously, so the slider IS the runtime control -- the dumpreq
 * tokens only tune the endpoints.
 *
 * Sign derivation (not a guess): an object behind the screen plane, viewed
 * from the right eye at +e/2, projects onto the screen at +x -- far content
 * shifts RIGHT in the right eye. p = k*(1 - C/d) with k > 0 for the right
 * eye gives exactly that. If hardware disagrees, `stereosep -12` is the A/B.
 */
int g_dbg_stereo = 1; /* master enable; the slider still gates frame by frame */
/*
 * WINDOW MODE: with pop-out allowed, the
 * clamp band turns the near ground into a constant-disparity sheet -- a
 * horizontal crease where graded road meets clamped road, which the owner
 * perceived as the ground "folding down" as it passes the kart. Window mode
 * forbids crossed disparity entirely: the clamp band's pop-out side becomes
 * 0, so the world starts AT the screen surface and only recedes. The flat
 * zone still exists but now coincides with the physical screen, which the
 * eye accepts as "the window frame" -- the standard comfortable-3DS
 * convention.
 *
 * Default OFF (owner ruling, same night he asked for it: "it's less cool").
 * Pop-out wins; the fold is the accepted price until a softer clamp knee
 * exists. dumpreq `stereowindow` turns the tame mode on.
 */
int g_dbg_stereoWindow = 0;
static C3D_RenderTarget *s_renderTargetRight = NULL;
static float s_stereoSep = 12.0f;    /* k*2 at full slider: total left/right disparity at infinite depth, PSX px */
static float s_stereoConv = 1200.0f; /* view.z units at screen depth (just beyond the player kart's ~256..2278 band) */
/* Pop-out clamp, as a multiple of the at-infinity shift k. 65536 means
 * uncapped: |p|/k = conv/d - 1 can never exceed it (conv's dumpreq clamp
 * tops out at 60000 and the shader floors d at 1), and 65536 * kMax still
 * fits PICA float24. dumpreq `stereopop N`; SELECT+D-LEFT toggles this
 * value capped <-> uncapped mid-race. */
#define N3DS_STEREO_POP_UNCAPPED 65536.0f
static float s_stereoPopMul = 2.0f;
static int s_stereoFrameActive = 0;  /* latched once per frame in N3DS_C3DBeginFrame */
static int s_stereoRecording = 0;    /* journal writes enabled: active frame, and cleared for
                                      * the replay so re-entered functions cannot re-record */
static int s_stereoEye = 0; /* 0 = left (and mono), 1 = right */
static float s_stereoVecWorld[2][4]; /* [eye][k, k*conv, clampLo, clampHi] -- track/model programs */
static float s_stereoVecPkt[2][4];   /* packet program: zeroed unless packet depth stamping is live */

/* Right eye from the left eye's command list (see N3DS_StereoCopyRightEye).
 * dumpreq `stereocopy [N]`; 0 always replays the journal. */
int g_dbg_stereoCopy = 1;
static void N3DS_StereoCopyOpenSpan(void);
static int N3DS_StereoCopyCheckLayout(void);

/*
 * The journal: one entry per draw-affecting call of the left-eye frame, in
 * call order. Replay re-invokes the same public functions (track/model), or
 * re-issues the resolved state + draw inline (packet/VRAM quads), against the
 * right target with the right eye's parallax uniform. Sized for the observed
 * shape of a frame (post-merge packet emits ~20-40, track leaf draws ~50-200,
 * model instances+runs ~100) with generous slack; overflow drops the tail of
 * the RIGHT eye only, loudly, never the left.
 */
enum
{
	N3DS_STEREO_OP_PKT = 0,
	N3DS_STEREO_OP_VRAM,
	N3DS_STEREO_OP_TRACK_BEGIN,
	N3DS_STEREO_OP_TRACK_LEAF,
	N3DS_STEREO_OP_TRACK_END,
	N3DS_STEREO_OP_MODEL_BEGIN,
	N3DS_STEREO_OP_MODEL_INSTANCE,
	N3DS_STEREO_OP_MODEL_RUN,
	N3DS_STEREO_OP_MODEL_END
};

typedef struct N3DS_StereoOp
{
	u8 kind;
	union
	{
		struct
		{
			u8 blend, doubling, depthWrite, atlas;
			s32 baseVertex, vertCount;
		} pkt; /* PKT; VRAM reuses atlas=slot, baseVertex */
		struct
		{
			float qx[4], qy[4], qw[4], h;
			u8 skyDome; /* MODEL_INSTANCE only */
		} mtx; /* TRACK_BEGIN, MODEL_INSTANCE */
		struct
		{
			s32 firstIndex, indexCount;
			float otIndexLo, ordinalLo, ordinalSpan, invIndexSpan;
			s32 atlasBin, blend, uvDx, uvDy;
		} leaf; /* TRACK_LEAF */
		struct
		{
			s32 startVertex, triCount, atlas, textured, blend;
		} mrun; /* MODEL_RUN */
	} u;
} N3DS_StereoOp;

#define N3DS_STEREO_MAX_OPS 1536
static N3DS_StereoOp s_stereoOps[N3DS_STEREO_MAX_OPS];
static int s_stereoOpCount = 0;
static int s_stereoOverflow = 0;

static N3DS_StereoOp *N3DS_StereoRecord(int kind)
{
	N3DS_StereoOp *op;

	if (!s_stereoRecording)
	{
		return NULL;
	}
	if (s_stereoOpCount >= N3DS_STEREO_MAX_OPS)
	{
		s_stereoOverflow = 1;
		return NULL;
	}
	op = &s_stereoOps[s_stereoOpCount++];
	op->kind = (u8)kind;
	return op;
}

/* Whether packet depth stamps currently carry real view.z (packet-depth
 * model) rather than emit ordinals. Same compile/runtime pair
 * N3DS_TrackDraw_Begin keys its own axis selector off, deliberately: parallax
 * from an ordinal stamp would be depth-shaped noise, so ordinal-stamped
 * builds/frames get a zero packet vector (flat packets at screen depth)
 * while track/model content -- whose depth is computed, not read from a
 * stamp -- still gets real parallax. */
static int N3DS_StereoPacketDepthActive(void)
{
	extern int g_dbg_packetDepth;

	return (g_dbg_packetDepth != 0);
}

/* Fills one eye's (k, k*conv, clampLo, clampHi). The clamp band is
 * asymmetric on purpose: pop-out (disparity crossed in front of the screen)
 * is allowed up to s_stereoPopMul times the at-infinity shift (default 2x),
 * then stops -- unbounded pop-out from very-near content (behind-eye track
 * clamped to depth 256, exhaust in the camera) is the classic eye-stabbing
 * failure of layered stereo. The into-screen bound stays at absK because
 * p = k*(1 - C/d) approaches k from below: that side is the curve's own
 * asymptote and never engages. */
static void N3DS_StereoEyeVec(float k, float *out4)
{
	const float absK = (k < 0.0f) ? -k : k;
	/* Window mode zeroes the crossed-disparity (pop-out) side of the clamp
	 * band; content nearer than the convergence depth pins to the screen
	 * plane instead of popping out and folding (see g_dbg_stereoWindow). */
	const float popOut = g_dbg_stereoWindow ? 0.0f : (s_stereoPopMul * absK);

	out4[0] = k;
	out4[1] = k * s_stereoConv;
	if (k >= 0.0f)
	{
		out4[2] = -popOut; /* right eye: near content shifts left */
		out4[3] = absK;
	}
	else
	{
		out4[2] = -absK;
		out4[3] = popOut;
	}
}

/* Widest horizontal shift stereo can apply to far-pinned content, in PSX
 * screen px, rounded up: p = k*(1 - conv/depth) tends to k at infinite depth,
 * and k = sep/2 at full slider. Flat sky content that must reach the screen
 * edges (backdrop fill, horizon glow) overscans X by this so neither eye's
 * shift can pull an edge on-screen; mono clips the overshoot at the window
 * edge. Deliberately constant while stereo is enabled -- NOT slider-tracking,
 * so the geometry never breathes with the slider. */
int N3DS_StereoSkyPadX(void)
{
	if (!g_dbg_stereo)
	{
		return 0;
	}
	return (int)(s_stereoSep * 0.5f) + 1;
}

/* Once per frame, from N3DS_C3DBeginFrame: read the physical slider, decide
 * whether this frame is stereo, reset the journal, compute both eyes'
 * vectors. Logs only on the active/inactive TRANSITION -- the slider is an
 * analog input and per-frame logging would grind the SD card. */
static void N3DS_StereoLatchFrame(void)
{
	static int s_wasActive = -1;
	float slider = 0.0f;

	s_stereoOpCount = 0;
	s_stereoOverflow = 0;
	s_stereoEye = 0;
	s_stereoFrameActive = 0;

	if (g_dbg_stereo && (s_renderTargetRight != NULL))
	{
		slider = osGet3DSliderState();
		if (slider > 0.01f)
		{
			s_stereoFrameActive = 1;
		}
	}
	s_stereoRecording = s_stereoFrameActive;

	{
		const float mag = s_stereoFrameActive ? (slider * s_stereoSep * 0.5f) : 0.0f;
		const int pktLive = s_stereoFrameActive && N3DS_StereoPacketDepthActive();
		int eye;

		for (eye = 0; eye < 2; eye++)
		{
			const float k = (eye == 1) ? mag : -mag;

			N3DS_StereoEyeVec(k, s_stereoVecWorld[eye]);
			N3DS_StereoEyeVec(pktLive ? k : 0.0f, s_stereoVecPkt[eye]);

			/*
			 * UI pop-out. NEGATIVE k: pop-out is CROSSED disparity, i.e. the
			 * opposite sign to the at-infinity shift (p = k - kC/d goes below k as
			 * content comes nearer, and reaches 0 at the convergence depth). One
			 * expression covers both eyes because k already carries the per-eye
			 * sign. Rides the packet vector's liveness so UI never pops out on a
			 * frame where packet depth is off and the band test is meaningless.
			 */
			s_stereoVecUi[eye][0] = (pktLive ? -k : 0.0f) * ((float)g_dbg_uiPopPct * 0.01f);

			/* Mirror mode's packet flip (ctr.v.pica): x' = x + (y + z*x) for world vertices, which is
			 * axisSum - x with these two and x itself with zeros. The world/UI test it relies on is
			 * the packet depth band, so it needs packet depth as the pop-out does. */
			s_stereoVecUi[eye][1] = (s_mirrorWorld && N3DS_StereoPacketDepthActive()) ? s_mirrorAxisSum : 0.0f;
			s_stereoVecUi[eye][2] = (s_mirrorWorld && N3DS_StereoPacketDepthActive()) ? -2.0f : 0.0f;
			s_stereoVecUi[eye][3] = 0.0f;
		}
	}

	if (s_stereoFrameActive != s_wasActive)
	{
		char msg[96];

		snprintf(msg, sizeof(msg), s_stereoFrameActive ? "[CTR-3DS] stereo: ACTIVE (slider=%d%% sep=%d conv=%d pktdepth=%d)" : "[CTR-3DS] stereo: inactive (slider down)",
		         (int)(slider * 100.0f), (int)s_stereoSep, (int)s_stereoConv, N3DS_StereoPacketDepthActive());
		N3DS_DebugLog(msg);
		s_wasActive = s_stereoFrameActive;
	}
}

/* Upload the current eye's parallax vector to one program's `stereo`
 * uniform. Guarded on the location like every other upload: -1 means the
 * lookup failed at init and writing would land on register 0. */
static void N3DS_StereoUploadUniform(int uLoc, const float (*table)[4])
{
	if (uLoc >= 0)
	{
		const float *v = table[s_stereoEye];

		C3D_FVUnifSet(GPU_VERTEX_SHADER, uLoc, v[0], v[1], v[2], v[3]);
	}
}

void N3DS_StereoRecordPacket(int blendMode, int doubling, int depthWrite, int atlas, int baseVertex, int vertCount)
{
	N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_PKT);

	if (op != NULL)
	{
		op->u.pkt.blend = (u8)blendMode;
		op->u.pkt.doubling = (u8)doubling;
		op->u.pkt.depthWrite = (u8)depthWrite;
		op->u.pkt.atlas = (u8)atlas;
		op->u.pkt.baseVertex = baseVertex;
		op->u.pkt.vertCount = vertCount;
	}
}

void N3DS_StereoRecordVram(int slot, int baseVertex)
{
	N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_VRAM);

	if (op != NULL)
	{
		op->u.pkt.atlas = (u8)slot;
		op->u.pkt.baseVertex = baseVertex;
		op->u.pkt.vertCount = 6;
	}
}

/* Top-screen widescreen mode. 0 = off (4:3 pillarbox crop).
 * 1 = STRETCH: lift the viewport crop only -- the ortho still maps the same
 *     window, so the same picture shows at wider pixels (anamorphic; kept as
 *     a dumpreq curiosity).
 * 2 = REAL: lift the crop AND extend the ortho window sideways by the same
 *     320->400 ratio (below), keeping PSX-units-per-panel-pixel IDENTICAL to
 *     4:3 mode. Perspective projection is planar, so content past the old
 *     window edges already projects to x<0 / x>dispW -- widening the window
 *     IS widening the FOV. The resident track/model renderers project on the
 *     GPU with no screen clamp, so their content genuinely fills the extra
 *     40 panel columns per side; anything CPU/retail-projected at 4:3
 *     (packets, UI) just lands centered at correct aspect. The wall is
 *     game-side 4:3 PVS/frustum culling: margins can show missing geometry.
 * Non-static: the renderer TU's stats screen and projection-rebuild check
 * read it across the plain-scalar TU boundary.
 *
 * Default 2 (REAL widescreen): the port boots widescreen by default.
 * The SELECT+START master flips it with the rest of the renderer config;
 * dumpreq `wide`/`wide 1` reaches all three states including the anamorphic
 * stretch. The known-open margin
 * classes (game-side 4:3 PVS/frustum culling holes, kart pop-in at the
 * margin edges) are unchanged by
 * this -- promoting the default does not close them, it just stops hiding
 * them behind a chord nobody presses. */
int g_dbg_widescreen = 2;

/* Punch-through alpha-test reference (0-255), applied by the single
 * C3D_AlphaTest below. Default 1 = "any non-zero", the established behaviour.
 * Raising it discards the partially-covered edge pixels bilinear invents at a
 * cutout boundary -- the dark rim over bright backgrounds (Labs grate over its
 * green floor, fences over the orange track). ONLY affects bilinear content:
 * nearest textures carry alpha exactly 0 or 255, which any reference in [1,255]
 * classifies identically, so models/UI/packets are untouched. Tunable live via
 * dumpreq `alpharef N` because the sweet spot (rim gone, thin grid lines not
 * yet eroded) is a look call, not a derivable constant. */
int g_dbg_trackAlphaRef = 1;

/*
 * Bounded C3D_FrameBegin with stall recovery (Rosalina-freeze hardening).
 * Rosalina's kernel extension freezes ALL app
 * threads AND the GSP sysmodule's own threads together (svcKernelSetState
 * 0x10000, mask 2|1), thawing both on menu close; HOME/lid suspend only
 * freeze the app side and thaw cleanly. The one place this port can wait
 * forever is inside C3D_FrameBegin itself -- C3Di_WaitAndClearQueue calls
 * gxCmdQueueWait(-1) unconditionally (SYNCDRAW only adds an extra untimed
 * C3D_FrameSync() ahead of it), which blocks in gspWaitForAnyEvent until the
 * GX queue's bound completion counters advance. A P3D/PPF completion event
 * lost across the freeze/thaw wedges that wait permanently.
 *
 * Every C3D_FrameBegin goes through N3DS_GuardedFrameBegin below -- a
 * bounded C3D_FRAME_NONBLOCK retry loop woken by a gsp event callback,
 * logging escalating stall lines instead of hanging silently.
 */

void N3DS_C3DSetProjection(float left, float right, float bottom)
{
	/* the widescreen growth below is symmetric, so the mirror axis is the same either way */
	s_mirrorAxisSum = left + right;

	if (g_dbg_widescreen == 2)
	{
		/* REAL widescreen: grow the window by exactly the viewport's
		 * 320->400 growth, split evenly, so 4:3 content keeps its position
		 * and scale and the margins become new world. At dispW=512 this is
		 * [-64, 576] -- 640 PSX columns across 400 panel px, the same 1.6
		 * units/px the 4:3 crop shows. */
		const float extra = (right - left) * ((400.0f - (float)N3DS_TOP_CONTENT_W) / (2.0f * (float)N3DS_TOP_CONTENT_W));

		left -= extra;
		right += extra;
	}
	/*
	 * PSX screen space -> clip space. There is no camera: the GTE already
	 * projected to 2D on the CPU, so this is a pure orthographic remap, and
	 * bottom/top are inverted (bottom > top) to reproduce PSX/PC's Y-down
	 * convention where Y=0 is the top of the image.
	 *
	 * This used to be hardcoded to the 3DS panel's own 400x240, which silently
	 * assumed PSX coordinates were already screen pixels. They are not: CTR
	 * runs a 512x216 DISPENV (SetDefDispEnv(..., 0x200, 0xd8) in MainMain.c),
	 * so a 1:1 passthrough cropped 112 columns -- 22% of the picture -- off the
	 * right edge and left a dead strip along the bottom. The extents now come
	 * from the game's live DISPENV instead; see NativeRenderer_BeginScene.
	 */
	/*
	 * Depth range covers the biased emit-ordinal in vertex z (s16, so
	 * [-32768, 32767] -- see N3DS_Vertex in n3ds_backend.h). Must span the
	 * full range: real z values outside near/far would be CLIPPED, so a
	 * narrow range plus stamped z would erase geometry, not just mis-layer
	 * it.
	 */
	Mtx_OrthoTilt(&s_projection, left, right, bottom, 0.0f, -32769.0f, 32769.0f, true);
}

/*
 * Rebinds the packet path's program + attribute layout + vertex buffer +
 * texture-modulate combiner. Split out of N3DS_C3DInit (which calls it once,
 * right after the one-time DVLB parse/shader-program-init/linear-vertex-
 * buffer-allocation steps below) so N3DS_TrackDraw_End (rung 3a step 2) can
 * call the SAME code to restore this state after the track draw bracket
 * switches program/attrInfo/bufInfo/env0 away from it for its own untextured
 * draws -- see that function's comment for why every piece of this has to
 * come back before the packet path's merge-bin flush runs. Redundant to call
 * during N3DS_C3DInit's own one-time setup (nothing has changed yet); cheap
 * either way, and having exactly one place that knows the packet path's full
 * GPU-state shape is worth the one extra call at boot.
 */
static void N3DS_BindPacketDrawState(void)
{
	C3D_AttrInfo *attrInfo;
	C3D_BufInfo *bufInfo;
	C3D_TexEnv *env;

	C3D_BindProgram(&s_shaderProgram);

	attrInfo = C3D_GetAttrInfo();
	AttrInfo_Init(attrInfo);
	/* v0 count is 4, not 3: x, y, z=occlusion depth, w=parallax depth (the
	 * vertex channel split -- n3ds_backend.h's N3DS_Vertex comment). Must track
	 * the struct's field count exactly; a mismatch is a hardware freeze class,
	 * not a visual bug. */
	AttrInfo_AddLoader(attrInfo, 0, GPU_SHORT, 4);         /* v0 = position (x, y, z=occlusion depth, w=parallax depth) */
	AttrInfo_AddLoader(attrInfo, 1, GPU_SHORT, 2);         /* v1 = texcoord (u, v) */
	AttrInfo_AddLoader(attrInfo, 2, GPU_UNSIGNED_BYTE, 3); /* v2 = color (r, g, b) -- alpha is a shader constant now, not fetched */

	bufInfo = C3D_GetBufInfo();
	BufInfo_Init(bufInfo);
	BufInfo_Add(bufInfo, s_vertexBufferMem, sizeof(N3DS_Vertex), 3, 0x210);

	/*
	 * Texture0 modulated by vertex color -- the CPU decode cache (design doc
	 * section 1) resolves page/CLUT/palette on the CPU, so by the time a
	 * texture reaches the GPU it is an ordinary RGBA8 image; the vertex color
	 * multiply reproduces the PC shader's v_color * texel behaviour (bright
	 * baked into vertex color CPU-side -- see native_renderer_3ds.c).
	 */
	env = C3D_GetTexEnv(0);
	C3D_TexEnvInit(env);
	C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	C3D_DirtyTexEnv(env);

	/*
	 * PICA200 vertex-shader float uniforms are physical registers shared by
	 * every bound program, not per-program handles. All three programs keep
	 * projection on f0-f3, stereo on f4 and stereoUi on f5, but the track and
	 * model programs write their own world vector to f4. Re-assert the packet
	 * projection/stereo uniforms here on every return to packet drawing, or
	 * the last bracket's values carry into the packet draws.
	 *
	 * Re-asserting it HERE rather than in N3DS_TrackDraw_End on purpose: this
	 * function is the single definition of "packet draw state", it is already
	 * called from every path that returns to packet drawing, and the
	 * projection is as much packet state as the program or the buffer layout.
	 * Guarded because C3D_C3DInit calls this before the location is resolved.
	 */
	if (s_uLocProjection >= 0)
	{
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	}

	/* The packet stereo vector shares f4 with the track and model programs'
	 * world vector, so it is packet state exactly like the projection rows
	 * above, re-asserted on every return to packet drawing. */
	N3DS_StereoUploadUniform(s_uLocStereo, (const float(*)[4])s_stereoVecPkt);
	/* UI pop-out rides the same re-upload, on f5. */
	N3DS_StereoUploadUniform(s_uLocStereoUi, (const float(*)[4])s_stereoVecUi);
}

/* Pulsed by N3DS_GxPulseCallback whenever GSP reports a P3D (command list
 * done) or PPF (display transfer done) completion -- the two events that
 * advance citro3d's bound GX queue. RESET_STICKY: a pulse that lands between
 * a waiter's last check and its next wait is not lost. Purely a wake-up
 * optimisation, not the correctness mechanism -- N3DS_GuardedFrameBegin
 * re-checks the real queue state (C3D_FrameBegin itself) on every timeout
 * regardless of whether this fired, so a missed or coalesced pulse costs at
 * most one poll period, never a hang. */
static LightEvent s_gxPulseEvent;

static void N3DS_GxPulseCallback(void *user)
{
	(void)user;
	LightEvent_Signal(&s_gxPulseEvent);
}

/* threadId for N3DS_BeatMark calls made from code that can legitimately run
 * on either the game thread or the render worker, depending on whether
 * NativeRenderQueue routing is active for this frame (CtrGpu_ExecDrawOTag
 * runs Platform_BeginScene -> ... -> N3DS_C3DBeginFrame on whichever thread
 * executes the frame's first DrawOTag/DrawPrim job). */
static int N3DS_BeatCurrentThreadId(void)
{
	return N3DS_RenderWorkerIsSelf() ? N3DS_BEAT_THREAD_RENDERWORKER : N3DS_BEAT_THREAD_MAIN;
}

/*
 * The port's only C3D_FrameBegin entry point (see the stall-guard comment
 * above for why it is guarded). The frame-begin goes through
 * C3D_FRAME_NONBLOCK so this function owns the only wait, timed against the
 * pulse event with a short poll period so a lost pulse costs milliseconds,
 * not a hang.
 *
 * Marks THIS thread's heartbeat FRAMEBEGIN for the span and back to OK on
 * return -- see n3ds_beat.h for why that has to be a single set/clear pair,
 * not a mark per retry.
 */
static void N3DS_GuardedFrameBegin(void)
{
	int beatThread;
	u64 startTick;
	u64 nextGateMs;
	unsigned int recoverN;

	beatThread = N3DS_BeatCurrentThreadId();
	N3DS_BeatMark(beatThread, N3DS_BEAT_WHERE_FRAMEBEGIN);
	startTick = svcGetSystemTick();

	nextGateMs = 1000;
	recoverN = 0;
	for (;;)
	{
		u64 waitedMs;

		if (C3D_FrameBegin(C3D_FRAME_NONBLOCK))
		{
			break;
		}

		waitedMs = ((svcGetSystemTick() - startTick) * 1000ull) / SYSCLOCK_ARM11;
		if (waitedMs >= nextGateMs)
		{
			char msg[96];

			recoverN++;
			/* vb0/vb1 moving across successive lines = GSP's event relay is
			 * alive and only this queue's completion was lost; both frozen =
			 * the relay itself is dead and nothing is recoverable from
			 * application code. */
			snprintf(msg, sizeof(msg), "[BEAT] GXRECOVER n=%u waitedMs=%u vb0=%lu vb1=%lu", recoverN, (unsigned)waitedMs,
			         (unsigned long)C3D_FrameCounter(0), (unsigned long)C3D_FrameCounter(1));
			N3DS_DebugLog(msg);
			N3DS_LogFlushNow();
			/*
			 * The documented recovery kick past this point would be
			 * gxCmdQueueInterrupt(GSPGPU_EVENT_P3D/PPF), faking the lost
			 * completion so the bound queue advances -- but it is not a
			 * public libctru symbol in the installed headers (no
			 * 3ds/gpu/gxqueue.h; gx.h declares only
			 * Clear/Add/Run/Stop/Wait/SetCallback + GX_BindQueue), so there
			 * is no supported way to call it. Recovery beyond logging is not
			 * possible from application code with this libctru; the wait
			 * keeps polling and self-heals if the completion was merely
			 * delayed rather than lost.
			 */
			nextGateMs = (recoverN < 3) ? (1000 + (u64)recoverN * 500) : (nextGateMs + 2000);
		}

		LightEvent_Clear(&s_gxPulseEvent);
		LightEvent_WaitTimeout(&s_gxPulseEvent, 4 * 1000 * 1000);
	}

	N3DS_BeatMark(beatThread, N3DS_BEAT_WHERE_OK);
}

int N3DS_C3DInit(void)
{
	/* 4x citro3d's 256KB default. Both hardware crashes to date
	 * (crash_dump_00000007; crash_dump_00000008) were
	 * the SAME deliberate panic: prefetch abort, IFSR=debug event, svcBreak
	 * from GPUCMD_AddInternal's overflow check (libctru gpu.c:33) under
	 * C3D_DrawArrays <- N3DS_FlushRun -- a real race's draw volume (multiple
	 * karts + effects; attract demos are lighter and never hit it) overflows
	 * the default command list mid-frame. 1MB of linear heap is cheap here
	 * (~6MB spare after the atlases) vs a hardware crash per overflow. */
	if (!C3D_Init(4 * C3D_DEFAULT_CMDBUF_SIZE))
	{
		return 0;
	}
	s_n3dsC3DActive = 1;

	/*
	 * Arm the gxguard wake source (see the stall-guard comment above) before
	 * anything can call N3DS_C3DBeginFrame. Slots are free -- citro3d itself
	 * only registers VBlank0/VBlank1 -- and oneShot=false since every frame
	 * needs the same callback. Runs on the gsp event thread, so it must do
	 * nothing but signal.
	 */
	LightEvent_Init(&s_gxPulseEvent, RESET_STICKY);
	gspSetEventCallback(GSPGPU_EVENT_P3D, N3DS_GxPulseCallback, NULL, false);
	gspSetEventCallback(GSPGPU_EVENT_PPF, N3DS_GxPulseCallback, NULL, false);
	N3DS_DebugLog("[BEAT] armed gxguard");

	s_renderTarget = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (s_renderTarget == NULL)
	{
		return 0;
	}
	C3D_RenderTargetSetOutput(s_renderTarget, GFX_TOP, GFX_LEFT, N3DS_DISPLAY_TRANSFER_FLAGS);

	/*
	 * Right-eye target, same shape as the left (another ~768KB of VRAM; the
	 * decode-cache atlases live in linear memory, so VRAM holds only these
	 * two targets and has room to spare). DELIBERATELY non-fatal on failure:
	 * a console that cannot allocate it boots mono with 3D permanently off
	 * and says so once, rather than refusing to boot over an optional
	 * feature. Every stereo path is gated on this pointer being non-NULL.
	 */
	s_renderTargetRight = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
	if (s_renderTargetRight != NULL)
	{
		C3D_RenderTargetSetOutput(s_renderTargetRight, GFX_TOP, GFX_RIGHT, N3DS_DISPLAY_TRANSFER_FLAGS);
	}
	else
	{
		N3DS_DebugLog("[CTR-3DS] stereo: right-eye render target FAILED to allocate, 3D disabled");
	}

	s_shaderDvlb = DVLB_ParseFile((u32 *)ctr_shbin, ctr_shbin_size);
	if (s_shaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_shaderProgram);
	shaderProgramSetVsh(&s_shaderProgram, &s_shaderDvlb->DVLE[0]);
	C3D_BindProgram(&s_shaderProgram);
	s_uLocProjection = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "projection");
	s_uLocStereo = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "stereo");
	s_uLocStereoUi = shaderInstanceGetUniformLocation(s_shaderProgram.vertexShader, "stereoUi");

	/*
	 * Placeholder until the first N3DS_C3DSetProjection call, which derives
	 * the real extents from the game's own DISPENV. See that function for why
	 * hardcoding screen pixels here was wrong.
	 */
	N3DS_C3DSetProjection(0.0f, 400.0f, 240.0f);

	s_vertexBufferMem = linearAlloc((size_t)N3DS_MAX_FRAME_VERTICES * sizeof(N3DS_Vertex));
	if (s_vertexBufferMem == NULL)
	{
		return 0;
	}

	/* Program + attribute layout + vertex buffer + texture-modulate combiner
	 * -- see N3DS_BindPacketDrawState's own comment for why this is a
	 * reusable function rather than inlined here (rung 3a step 2 needs to
	 * restore this exact state after the track draw bracket). */
	N3DS_BindPacketDrawState();

	/*
	 * Visibility sentinel. The decode cache encodes alpha as a single bit,
	 * with WHICH texels carry it decided per-cell at decode time (the
	 * N3DS_TexView per-texel STP views: punch-through for
	 * opaque cells, the STP split for semi-trans pass-1/pass-2 cells). One
	 * GEQUAL-1 test therefore serves every pass unchanged -- the pass
	 * distinction lives in the sampled cell, not in GPU state. Set once
	 * here rather than per-draw since no draw ever needs a different test.
	 * Reference is g_dbg_trackAlphaRef (default 1) -- see its comment; raising
	 * it trims bilinear's cutout fringe and is a no-op for nearest content.
	 */
	C3D_AlphaTest(true, GPU_GEQUAL, (int)g_dbg_trackAlphaRef);

	/*
	 * Fixed blend color shared by BM_AVERAGE (reads the alpha, 0.5) and
	 * BM_ADD_QUATER_SOURCE (reads the RGB, 0.25) -- the same dual-use
	 * constant PC sets with glBlendColor(0.25, 0.25, 0.25, 0.5).
	 * GPUREG_BLEND_COLOR is R in bits 0-7 through A in bits 24-31.
	 * Set once: no mode ever needs a different constant, so
	 * N3DS_C3DSetBlendMode only ever touches equation/factors.
	 */
	C3D_BlendingColor(0x80404040u);
	N3DS_C3DSetBlendMode(0);

	C3D_CullFace(GPU_CULL_NONE);

	/*
	 * No depth testing, matching the PC renderer's explicit
	 * glDisable(GL_DEPTH_TEST) (native_renderer.c ~1880). This renderer is
	 * pure painter's algorithm: the PS1 ordering table sorts whole polygons
	 * on the CPU and draw order alone decides layering, which is why
	 * DrawTriangles is careful never to reorder triangles.
	 *
	 * Setting this explicitly is load-bearing, not tidiness. Every vertex
	 * leaves the shader at the SAME depth (ctr.v.pica forces z=0), so if
	 * citro3d's default happened to enable testing with GPU_GREATER, every
	 * fragment after the first at a given pixel would compare equal, fail,
	 * and be discarded -- silently inverting layering so that whatever drew
	 * FIRST wins instead of last. Relying on an unverified library default
	 * for that is not worth it when PC states the intent outright.
	 */
	C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);

	{
		int i;
		char msg[128];

		/* Atlases are the single largest linear-memory consumer here
		 * (N3DS_ATLAS_DIM^2 * 2 bytes each, GPU_RGBA5551). Log the budget
		 * before and after so cache capacity can be sized from the real
		 * ceiling instead of guessed -- see N3DS_ATLAS_COUNT's own comment
		 * in n3ds_backend.h for the current count and why; a stale version
		 * of this comment used to claim a specific count "failed outright",
		 * which was true only for the twice-as-large RGBA8 format this no
		 * longer uses. If allocation ever fails now, the log line below
		 * states exactly which atlas index and how much free KiB remained --
		 * trust that over any comment. */
		snprintf(msg, sizeof(msg), "[CTR-3DS] linear free before atlases: %lu KiB (want %d x %d KiB)",
		         (unsigned long)(linearSpaceFree() / 1024), N3DS_ATLAS_COUNT,
		         (N3DS_ATLAS_DIM * N3DS_ATLAS_DIM * 2) / 1024);
		N3DS_DebugLog(msg);

		for (i = 0; i < N3DS_ATLAS_COUNT; i++)
		{
			if (!C3D_TexInit(&s_atlasTextures[i], N3DS_ATLAS_DIM, N3DS_ATLAS_DIM, GPU_RGBA5551))
			{
				snprintf(msg, sizeof(msg), "[CTR-3DS] atlas %d of %d FAILED to allocate; linear free %lu KiB", i,
				         N3DS_ATLAS_COUNT, (unsigned long)(linearSpaceFree() / 1024));
				N3DS_DebugLog(msg);
				return 0;
			}
			C3D_TexSetFilter(&s_atlasTextures[i], GPU_NEAREST, GPU_NEAREST);
			C3D_TexSetWrap(&s_atlasTextures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
	}

	/* Boot-splash present texture pool -- see the header comment on
	 * N3DS_C3DUploadVramDisplayChunk for why this is a small pool, not one
	 * shared texture like this loop might suggest. */
	{
		int slot;

		for (slot = 0; slot < N3DS_VRAM_DISPLAY_TEX_COUNT; slot++)
		{
			if (!C3D_TexInit(&s_vramDisplayTex[slot], N3DS_VRAM_DISPLAY_TEX_DIM, N3DS_VRAM_DISPLAY_TEX_DIM, GPU_RGBA5551))
			{
				N3DS_DebugLog("[CTR-3DS] VRAM display texture FAILED to allocate");
				return 0;
			}
			/* Linear: the 512-wide display shrinks to 320 panel px, and
			 * nearest sampling skips 3 of every 8 columns, which breaks
			 * up the splash screens' small text. */
			C3D_TexSetFilter(&s_vramDisplayTex[slot], GPU_LINEAR, GPU_LINEAR);
			C3D_TexSetWrap(&s_vramDisplayTex[slot], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
		}
	}

	/* Rung 3a step 2's sibling shader/program -- a SECOND program object, not
	 * a second vertex shader in the same one, since the packet and track
	 * paths need to switch attribute layout and uniform sets independently
	 * (N3DS_TrackDraw_Begin/End bind whichever one is active). Failure here
	 * is fatal to init, same as the packet shader above: a track-renderer
	 * build with no track shader would silently draw nothing once the
	 * toggle is flipped on, which is worse than failing loudly at boot. */
	s_trackShaderDvlb = DVLB_ParseFile((u32 *)ctr_track_shbin, ctr_track_shbin_size);
	if (s_trackShaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_trackShaderProgram);
	shaderProgramSetVsh(&s_trackShaderProgram, &s_trackShaderDvlb->DVLE[0]);
	s_trackULocQx = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "qx");
	s_trackULocQy = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "qy");
	s_trackULocQw = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "qw");
	s_trackULocProjection = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "projection");
	s_trackULocH = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "hParam");
	s_trackULocDepthRange = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "depthRange");
	s_trackULocUvOffset = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "uvOffset");
	s_trackULocStereo = shaderInstanceGetUniformLocation(s_trackShaderProgram.vertexShader, "stereo");

	/* Fixed attribute layout for N3DS_TrackVertex -- does not depend on any
	 * per-level allocation (unlike s_trackBufInfo, built in
	 * N3DS_TrackGeom_Upload once a level's vertex buffer address is known),
	 * so it is safe to build once here. v2's count is 4, not 3 like the
	 * packet path's color loader -- drawOrder rides as the 4th fetched byte
	 * (see N3DS_TrackVertex's own comment for why it can't be a signed
	 * GPU_BYTE group instead). */
	AttrInfo_Init(&s_trackAttrInfo);
	AttrInfo_AddLoader(&s_trackAttrInfo, 0, GPU_SHORT, 3);         /* v0 = position (x, y, z) */
	AttrInfo_AddLoader(&s_trackAttrInfo, 1, GPU_SHORT, 2);         /* v1 = texcoord (u, v) -- unused in 3a */
	AttrInfo_AddLoader(&s_trackAttrInfo, 2, GPU_UNSIGNED_BYTE, 4); /* v2 = color + biased drawOrder */

	/* Rung 2's program. Fatal on failure for the same reason the other two
	 * are: a model-renderer build with no model shader would draw nothing the
	 * moment the toggle is flipped, and "nothing appeared" is a far more
	 * expensive thing to debug than a loud boot failure.
	 *
	 * NO AttrInfo built here. Model draws reuse the packet layout, which
	 * N3DS_BindPacketDrawState installs; building a second identical one would
	 * be a copy that can silently drift from the layout the vertices are
	 * actually written in. */
	s_modelShaderDvlb = DVLB_ParseFile((u32 *)ctr_model_shbin, ctr_model_shbin_size);
	if (s_modelShaderDvlb == NULL)
	{
		return 0;
	}
	shaderProgramInit(&s_modelShaderProgram);
	shaderProgramSetVsh(&s_modelShaderProgram, &s_modelShaderDvlb->DVLE[0]);
	s_modelULocProjection = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "projection");
	s_modelULocQx = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "qx");
	s_modelULocQy = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "qy");
	s_modelULocQw = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "qw");
	s_modelULocH = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "hParam");
	s_modelULocStereo = shaderInstanceGetUniformLocation(s_modelShaderProgram.vertexShader, "stereo");

	if (!N3DS_StereoCopyCheckLayout())
	{
		N3DS_DebugLog("[CTR-3DS] stereo copy: shader uniform layout check FAILED, right eye replays the journal");
	}

	return 1;
}

void N3DS_C3DExit(void)
{
	int i;

	N3DS_TrackGeom_Free();
	shaderProgramFree(&s_trackShaderProgram);
	if (s_trackShaderDvlb != NULL)
	{
		DVLB_Free(s_trackShaderDvlb);
		s_trackShaderDvlb = NULL;
	}

	for (i = 0; i < N3DS_VRAM_DISPLAY_TEX_COUNT; i++)
	{
		C3D_TexDelete(&s_vramDisplayTex[i]);
	}

	for (i = 0; i < N3DS_ATLAS_COUNT; i++)
	{
		C3D_TexDelete(&s_atlasTextures[i]);
	}

	if (s_vertexBufferMem != NULL)
	{
		linearFree(s_vertexBufferMem);
		s_vertexBufferMem = NULL;
	}

	shaderProgramFree(&s_shaderProgram);
	if (s_shaderDvlb != NULL)
	{
		DVLB_Free(s_shaderDvlb);
		s_shaderDvlb = NULL;
	}

	C3D_Fini();
	s_n3dsC3DActive = 0;
}

/*
 * DIAGNOSTIC: real hardware can freeze inside the first boot-splash present
 * with no further output. The two blocking candidates are C3D_FrameBegin's
 * SYNCDRAW wait (previous command list never completed) and C3D_FrameEnd's
 * queue handoff. Trace the first few frame brackets so the stream names the
 * exact call that never returns; bounded so steady-state runs don't flood
 * sdmc:/ctr3ds.log.
 */
static int s_frameTraceBudget = 48;

static void N3DS_FrameTrace(const char *msg)
{
	if (s_frameTraceBudget > 0)
	{
		s_frameTraceBudget--;
		N3DS_DebugLog(msg);
	}
}

/*
 * g_dbg_depthTest isolates whether the depth test (state 1) or the track's
 * own depth writes (state 2) hide non-track content. Every state is a VISUAL
 * binary a human settles in one glance -- deliberately not a counter, for the
 * reason PORT_FEASIBILITY_3DS.md's "How to measure" section exists:
 *
 *   0 NORMAL  -- LEQUAL + depth writes. Today's behaviour, the baseline.
 *   1 NOTEST  -- GPU_ALWAYS + depth writes: pure submission (painter's) order.
 *                Content reappears  => the depth TEST is what hides it, so
 *                                      this is a depth-VALUE bug.
 *                Content still gone  => depth is irrelevant and the content is
 *                                      never drawn at all -- look at what
 *                                      skipping DrawLevelOvr1P took with it
 *                                      (MainFrame_RenderFrame.c's
 *                                      skipRetailTrackDraw), starting with the
 *                                      skybox, which that call does NOT draw
 *                                      and which is missing anyway.
 *   2 TRACKRO -- normal test, but the TRACK draws read-only against depth
 *                (GPU_WRITE_COLOR, applied in N3DS_TrackDraw_Begin). Isolates
 *                the track's own depth WRITES from every other variable: if
 *                only this state restores the karts, the track is stamping
 *                depth that outranks them, and the packet side is fine.
 *
 * State 1 vs 2 is the whole point -- 1 asks "is it depth at all", 2 asks "is it
 * OUR depth". Runtime, because the answer is worth more than a rebuild cycle:
 * dumpreq `depthtest [N]`.
 */
int g_dbg_depthTest = 0;

/*
 * s32 near-band polarity (ctr_track.v.pica's NEAR-BAND POLARITY block). 0 is
 * the pre-s32 build bit-for-bit; 1 sends track vertices at or behind the eye
 * to the FAR end of the depth axis instead of the near end.
 *
 * Lives in THIS translation unit like g_dbg_depthTest, because both of its
 * readers (the hParam uniform write and the dumpreq handler) are here.
 *
 * dumpreq `nearfar` only -- deliberately NOT a button. Unlike a perf A/B,
 * this one does not need a mid-race flip: the
 * sink is visible in a still frame, so booting once each way settles it, and a
 * dumpreq IS placeable before launch even on hardware.
 *
 * DEFAULT 0, INCLUDING in demo builds. It briefly rode
 * CTR_NATIVE_RENDERER_DEFAULT_ON so a watch-only sitting would see it armed;
 * the owner then watched that build and reported racers still sunk **on
 * sweeping aerial shots**, which falsifies the theory outright -- from a
 * camera high above the track there is no geometry at or behind the eye for
 * this to touch, so it cannot be what buries anything. A sink that is
 * unchanged by camera distance and angle is a CONSTANT OFFSET, a different
 * failure entirely. Kept (it is still strictly more correct than clamping
 * behind-eye vertices to maximally-near) but off, and no longer allowed to
 * ride along in a build where something else is being judged.
 */
/*
 * Preset ladder for the packet depth bias (see g_dbg_pktDepthBias, native_gpu.c).
 * Values are view.z units pulled toward the camera. Deliberately coarse and
 * short: this is dialled BY EYE mid-race on hardware, where a fine-grained
 * control the owner has to press twenty times is useless. 256 is the top rung
 * because it equals the track's own near-band clamp -- past that, packet
 * content starts winning against track surfaces it is genuinely behind, which
 * is the "racers through walls" failure the owner explicitly ruled out.
 * Shared by the hardware chord and the dumpreq token so the two agree.
 */
static int N3DS_NextPktBias(int cur)
{
	static const int ladder[5] = {0, 32, 64, 128, 256};
	int i;

	for (i = 0; i < 5; i++)
	{
		if (ladder[i] == cur)
		{
			return ladder[(i + 1) % 5];
		}
	}
	return 0; /* off-ladder (set via 'pktbias N') rejoins at the bottom */
}

/*
 * Frame builds begin the moment the GPU is free rather than aligning to the
 * hardware vblank (C3D_FrameBegin(SYNCDRAW)): the alignment is native-only
 * overhead, not pacing, since the 30fps cap is enforced by the game itself
 * (ReadyToFlip counts the emulated 60Hz clock, not phase-locked to the panel)
 * and presentation is vblank-latched by GSP regardless, so nothing tears
 * without it. Hardware-measured: Tubes mono 41->34.7ms, stereo 49->36.2ms --
 * the wait rounded frames up to the next vblank multiple.
 */

void N3DS_SetMirrorWorld(int on)
{
	s_mirrorWorld = (on != 0);
}

void N3DS_C3DBeginFrame(void)
{
	/* Latch the physical 3D slider for the whole frame BEFORE anything is
	 * cleared or uploaded: the latch decides whether the right target gets
	 * cleared, which parallax the left eye's own uniforms carry, and whether
	 * the draw journal records. Pure CPU, so it sits ahead of the SYNCDRAW
	 * wait rather than inside the frame bracket. */
	N3DS_StereoLatchFrame();

	N3DS_FrameTrace("[CTR-3DS] FrameBegin: enter (SYNCDRAW wait)");
	N3DS_GuardedFrameBegin();
	N3DS_FrameTrace("[CTR-3DS] FrameBegin: returned");
	/* Depth clears to FAR. Mtx_OrthoTilt maps LARGER vertex z to SMALLER
	 * stored depth (verified empirically, see below), so far = max. */
	C3D_RenderTargetClear(s_renderTarget, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
	if (s_stereoFrameActive)
	{
		/* Only on stereo frames: a cleared-but-never-drawn right target
		 * would still cost the clear, and with the barrier off nothing
		 * presents it anyway. */
		C3D_RenderTargetClear(s_renderTargetRight, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
	}
	C3D_FrameDrawOn(s_renderTarget);

	/*
	 * Depth substrate: every prim carries its OT-emission ordinal as z, so
	 * later-submitted always wins the depth test -- painter's order in the
	 * depth buffer, making submission REORDER legal for the cross-bucket
	 * merge. Polarity was settled EMPIRICALLY and the first guess (GEQUAL +
	 * clear 0) was wrong in an instructive way: inverted layering does NOT
	 * look broken in back-to-front world scenes (far-wins camouflages as
	 * plausible occlusion) -- it announces itself by ERASING late-drawn
	 * content: the loading flag's LOADING letters, the DEMO MODE banner,
	 * the karts. Mtx_OrthoTilt maps larger vertex z to smaller stored
	 * depth, so the correct pair is LEQUAL + clear-to-max. If this ever
	 * needs re-deriving, use the loading screen's letters as the canary,
	 * not world layering.
	 */
	/* State 2 (TRACKRO) leaves this at the normal pair -- it only narrows the
	 * write mask for the track's own draws, inside N3DS_TrackDraw_Begin. */
	if (g_dbg_depthTest == 1)
	{
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	}
	else
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}

	/*
	 * Pillarbox the top screen to the bottom screen's 4:3-equivalent width.
	 * Top panel is 400 wide, bottom is 320 -- PSX content is
	 * meant for 4:3, and letting it fill the full 400 (what the ortho range
	 * alone can't prevent, see NativeRenderer_BeginScene) shows ~40 extra PSX
	 * columns of world per edge that were never meant to be on screen.
	 *
	 * The render target is stored sideways, 240x400 (see
	 * C3D_RenderTargetCreate above), for the 3DS's rotated panel, so
	 * screen-X is this buffer's Y axis, not X. A *centred* crop is symmetric
	 * either way that rotation runs, so which physical edge is which doesn't
	 * matter here: (400-320)/2 = 40px margin, full 240 untouched on the
	 * buffer's other axis (both screens are 240 tall, no vertical crop).
	 * Fixed hardware constants, not DISPENV-dependent, so set once a frame
	 * here rather than threaded through as a parameter.
	 *
	 * N3DS_TOP_CONTENT_* (n3ds_backend.h) mirror this crop for anything that
	 * reads the presented frame back -- keep them in lockstep with this call
	 * (N3DS_CopyDisplayedTopFrame samples only the content region, else the
	 * pause background re-ingests the black bars and shows them squeezed
	 * inside the picture -- user-reported on hardware).
	 *
	 * Any nonzero g_dbg_widescreen lifts the crop. Mode 1 alone is an
	 * anamorphic STRETCH; mode 2 (REAL) pairs the lift with the extended
	 * ortho window in N3DS_C3DSetProjection -- see the flag's definition.
	 * KNOWN while wide: the pause-background readback still samples the
	 * 320-wide CONTENT region, so the paused backdrop is center-cropped;
	 * and full-screen packet quads (fades, pause dim) cover only the
	 * centered 4:3 band, leaving the margins undimmed.
	 */
	C3D_SetViewport(0, g_dbg_widescreen ? 0 : N3DS_TOP_CONTENT_X, 240, g_dbg_widescreen ? 400 : N3DS_TOP_CONTENT_W);

	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	/* Left eye's parallax (zero in mono) -- rides beside the projection for
	 * the same reason it does in N3DS_BindPacketDrawState. */
	N3DS_StereoUploadUniform(s_uLocStereo, (const float(*)[4])s_stereoVecPkt);
	/* UI pop-out rides the same re-upload, on f5. */
	N3DS_StereoUploadUniform(s_uLocStereoUi, (const float(*)[4])s_stereoVecUi);

	/* Every draw this frame appends into the SAME persistent buffer at a
	 * fresh offset (see the header comment on N3DS_C3DAppendVertices) --
	 * reset the write cursor once per frame, not per draw. */
	s_vertexCursor = 0;

	/* Last: the left eye's command span starts after this frame's state is
	 * flagged, so its first draw re-emits all of it. */
	N3DS_StereoCopyOpenSpan();
}

/* GPU-time window accumulator (see n3ds_backend.h). Written on whichever
 * thread ends frames (the render worker when the queue is live), fetched from
 * the game thread once per [PERF] window -- a torn read costs one frame of a
 * 64-frame average, so no lock. */
static float s_gtCpuMs;
static float s_gtGpuMs;
static unsigned s_gtFrames;

void N3DS_C3DEndFrame(void)
{
	/* C3D_FrameEnd only submits the command list -- it does not itself wait
	 * for the GPU (that wait is deferred to the NEXT frame's
	 * N3DS_GuardedFrameBegin) -- but it is bracketed the same way so a future
	 * libctru version that blocks here is not a silent hole in the coverage. */
	const int beatThread = N3DS_BeatCurrentThreadId();

	N3DS_FrameTrace("[CTR-3DS] FrameEnd: enter (submit)");
	N3DS_BeatMark(beatThread, N3DS_BEAT_WHERE_FRAMEEND);
	C3D_FrameEnd(0);
	N3DS_BeatMark(beatThread, N3DS_BEAT_WHERE_OK);
	N3DS_FrameTrace("[CTR-3DS] FrameEnd: returned");

	/* Read here rather than in BeginFrame so the pair is sampled once per
	 * real rendered frame. cpuTime is stamped inside C3D_FrameEnd for the
	 * frame just submitted; gpuTime is stamped by the GX queue-finish
	 * callback, so it belongs to the PREVIOUS completed frame -- a one-frame
	 * skew that vanishes in a windowed average. */
	s_gtCpuMs += C3D_GetProcessingTime();
	s_gtGpuMs += C3D_GetDrawingTime();
	s_gtFrames++;
}

void N3DS_C3DGpuTimeWindow(int *cpuUs, int *gpuUs, unsigned *frames)
{
	unsigned n = s_gtFrames;

	*cpuUs = (n != 0) ? (int)((s_gtCpuMs * 1000.0f) / (float)n) : 0;
	*gpuUs = (n != 0) ? (int)((s_gtGpuMs * 1000.0f) / (float)n) : 0;
	*frames = n;
	s_gtCpuMs = 0.0f;
	s_gtGpuMs = 0.0f;
	s_gtFrames = 0;
}

int N3DS_C3DAppendVertices(const N3DS_Vertex *vertices, int count)
{
	int base;
	N3DS_Vertex *dst;

	if ((count <= 0) || ((s_vertexCursor + count) > N3DS_MAX_FRAME_VERTICES))
	{
		return -1;
	}

	base = s_vertexCursor;
	dst = (N3DS_Vertex *)s_vertexBufferMem + base;
	memcpy(dst, vertices, (size_t)count * sizeof(N3DS_Vertex));
	s_vertexCursor += count;

	if (s_vertexHighWater < s_vertexCursor)
	{
		s_vertexHighWater = s_vertexCursor;
	}

	return base;
}

int N3DS_C3DFrameVertexHighWater(void)
{
	return s_vertexHighWater;
}

void N3DS_C3DDrawTriangles(int startVertex, int triangleCount)
{
	C3D_DrawArrays(GPU_TRIANGLES, startVertex, triangleCount * 3);
}

/*
 * 8x8 texels * 2 bytes/texel (RGBA5551) -- one PICA200 tile, tiled
 * contiguously.
 *
 * Previously RGBA8. The decode cache encoded a three-level alpha
 * (0 punch-through / 128 opaque / 255 STP-on) but NOTHING ever read the
 * 128-vs-255 distinction: the only alpha test in this file is a single
 * C3D_AlphaTest(true, GPU_GEQUAL, 1), i.e. zero-vs-nonzero. So half of
 * every cell's memory was carrying a distinction no code consumed.
 *
 * Halving the texel doubles the number of cache cells the same linear memory
 * buys, and cell COUNT was the measured bottleneck: playing the menu showed
 * 1441 capacity misses per 32-frame window (~45/frame) against a saturated
 * 63/63 working set, plus 2052 evictions of cells a queued draw still
 * referenced -- which is the visible texture corruption. The STP two-pass
 * blend recovered the STP bit exactly as predicted here: from
 * VRAM at decode time, baked into per-view cells (N3DS_TexView), keeping
 * this format one bit of alpha.
 */
#define N3DS_TILE_BYTES (64 * 2)

/*
 * Manual sub-rect write -- see the header comment for why C3D_TexUpload
 * can't be used here. PICA200 tiling addresses 8x8-texel tiles in simple
 * row-major order across a texture's FULL width (verified against
 * tex3ds's reference swizzle, see native_renderer_3ds.c). A tile block is a
 * tilesWide x tilesHigh square positioned at tile offset (baseTileCol,
 * baseTileRow) within the destination texture's own tile grid
 * (tilesPerRow x tilesPerRow); each of its tile-rows is tilesWide
 * CONSECUTIVE tiles, which -- because tile-column is the fastest-varying
 * index in row-major order -- are contiguous in the texture's memory too,
 * so one memcpy per tile-row is exact and correct, not an approximation.
 *
 * Shared by the whole-cell (32x32-tile) and sub-cell (8x8-tile) decode-cache
 * upload wrappers below, AND by the
 * native track renderer's dedicated atlas pool (N3DS_TrackAtlas_UploadRect,
 * rung 2b) -- generalized from a hardcoded s_atlasTextures[]/
 * N3DS_ATLAS_DIM to an explicit destination texture + tiles-per-row so both
 * pools can share one blit implementation rather than duplicating it.
 * tilesWide/tilesHigh must be used for ALL FOUR of the base-tile calc (the
 * caller's job here, via baseTileCol/baseTileRow), the loop bound, the
 * srcRow stride, and the memcpy width -- getting only the loop bound right
 * and leaving srcRow/memcpy at a stale hardcoded stride was caught in review
 * before this shipped (correct only for a 32-wide caller, silently wrong for
 * an 8-wide one).
 */
static void N3DS_C3DUploadTileBlock(C3D_Tex *tex, int tilesPerRow, int baseTileCol, int baseTileRow, int tilesWide,
                                     int tilesHigh, const void *tiled)
{
	const u8 *src = (const u8 *)tiled;
	u8 *texData;
	int localTileRow;

	if (tex == NULL)
	{
		return;
	}

	texData = (u8 *)tex->data;

	for (localTileRow = 0; localTileRow < tilesHigh; localTileRow++)
	{
		const int dstTileRow = baseTileRow + localTileRow;
		u8 *dstRow = texData + (size_t)((dstTileRow * tilesPerRow) + baseTileCol) * N3DS_TILE_BYTES;
		const u8 *srcRow = src + (size_t)localTileRow * tilesWide * N3DS_TILE_BYTES;

		memcpy(dstRow, srcRow, (size_t)tilesWide * N3DS_TILE_BYTES);
	}

	/*
	 * Flush the just-written region to physical RAM. Without this the GPU (and,
	 * critically, Azahar's/Citra's host-side texture cache) never learns the
	 * texture's content changed: after a cell is evicted and re-decoded for a
	 * new texture, the emulator keeps serving the cell's PREVIOUS occupant, which
	 * is exactly the "recognizable textures showing up in the wrong places on the
	 * track" corruption. This is the missing counterpart to a C3D_TexUpload that
	 * the raw-memcpy upload path above never had. Flush the bounding range of
	 * the rows written (over-flushing the inter-row gaps is harmless -- the data
	 * there is already correct, and a slightly larger invalidation just re-reads
	 * a little more of the texture). GSPGPU_FlushDataCache rounds to cache lines
	 * internally, so an unaligned start/size is fine. */
	{
		u8 *flushStart = texData + (size_t)((baseTileRow * tilesPerRow) + baseTileCol) * N3DS_TILE_BYTES;
		size_t flushBytes = (size_t)(((tilesHigh - 1) * tilesPerRow) + tilesWide) * N3DS_TILE_BYTES;

		GSPGPU_FlushDataCache(flushStart, (u32)flushBytes);
	}
}

void N3DS_C3DUploadDecodedCell(int atlasIndex, int cellX, int cellY, const void *tiled256)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}
	N3DS_C3DUploadTileBlock(&s_atlasTextures[atlasIndex], N3DS_ATLAS_DIM / 8, cellX * 32, cellY * 32, 32, 32, tiled256); /* unchanged behavior */
}

void N3DS_C3DUploadDecodedSubCell(int atlasIndex, int physCellX, int physCellY, int subX, int subY, const void *tiled64)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}
	N3DS_C3DUploadTileBlock(&s_atlasTextures[atlasIndex], N3DS_ATLAS_DIM / 8, (physCellX * 32) + (subX * 8), (physCellY * 32) + (subY * 8), 8, 8,
	                         tiled64);
}

#ifdef CTR_INTERNAL
/* Dumps every atlas texture's raw (8x8-Morton-tiled RGBA5551) linear storage to
 * sdmc:/atlas_<i>.bin for the texture-corruption investigation.
 * Un-swizzled + rendered on the host. Runs inside the dumpreq `capture` set
 * alongside the VRAM dump so both capture the same frame. s_atlasTextures[].data is CPU-written by
 * the decode (N3DS_C3DUploadTileBlock), so it is CPU-readable here. */
static void N3DS_DebugWriteAtlasDump(void)
{
	const size_t bytes = (size_t)N3DS_ATLAS_DIM * (size_t)N3DS_ATLAS_DIM * 2;
	char path[64];
	char msg[96];
	int i, written = 0;

	for (i = 0; i < N3DS_ATLAS_COUNT; i++)
	{
		FILE *f;

		if (s_atlasTextures[i].data == NULL)
		{
			continue;
		}

		snprintf(path, sizeof(path), "sdmc:/atlas_%u_%d.bin", N3DS_DebugCaptureFrame(), i);
		f = fopen(path, "wb");
		if (f == NULL)
		{
			continue;
		}

		fwrite(s_atlasTextures[i].data, 1, bytes, f);
		fclose(f);
		written++;
	}

	snprintf(msg, sizeof(msg), "[CTR-3DS] dumped %d/%d atlas textures (%u bytes each)", written, N3DS_ATLAS_COUNT, (unsigned)bytes);
	N3DS_DebugLog(msg);
}
#endif

void N3DS_C3DSetColorDoubling(int enable)
{
	/*
	 * PC's "bright" multiplier, done the way PC does it. Its vertex shader
	 * computes v_color.xyz = (color/255) * bright with NO clamp, and the
	 * fragment shader clamps only at the final write, AFTER multiplying by
	 * the texel (native_renderer.c: "v_color.xyz *= a_texcoord.z", then
	 * "fragColor = dither(color * v_color)").
	 *
	 * Baking bright into the vertex colour CPU-side cannot reproduce that:
	 * a vertex colour is clamped to 1.0 before it ever reaches the multiply,
	 * so all the headroom above 1.0 is lost and every lit textured surface
	 * comes out darker than on PC. The TEV output scale is applied AFTER the
	 * modulate, which restores PC's exact ordering.
	 *
	 * RGB only -- deliberately NOT C3D_Both. PC scales only .xyz; doubling
	 * alpha as well would push the punch-through-vs-opaque bit from 1 to 2,
	 * i.e. out of range for the 1-bit alpha channel (see N3DS_TILE_BYTES),
	 * corrupting the punch-through alpha test in n3ds_backend.c's C3D_AlphaTest.
	 */
	C3D_TexEnv *env = C3D_GetTexEnv(0);

	C3D_TexEnvScale(env, C3D_RGB, enable ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1);
	/* Explicit, not redundant-by-inspection: C3D_TexEnvScale is a plain
	 * inline struct write, and citro3d exposes C3D_DirtyTexEnv precisely
	 * because a modified env is not always flagged for re-emission. Without
	 * a dirty flag the change never reaches the GPU and this whole function
	 * is a silent no-op. Cheap to state outright rather than depend on
	 * C3D_GetTexEnv's internals, which aren't visible from the headers. */
	C3D_DirtyTexEnv(env);
}

void N3DS_C3DBindAtlas(int atlasIndex)
{
	if ((atlasIndex < 0) || (atlasIndex >= N3DS_ATLAS_COUNT))
	{
		return;
	}

	C3D_TexBind(0, &s_atlasTextures[atlasIndex]);
}

/* ------------------------------------------------ native track atlas --- */
/* See the header comment (n3ds_backend.h) for the pool's rationale. */

void N3DS_TrackAtlas_Free(void)
{
	int i;

	for (i = 0; i < s_trackAtlasUnitsAllocated; i++)
	{
		C3D_TexDelete(&s_trackAtlasTextures[i]);
	}
	s_trackAtlasUnitsAllocated = 0;
}

/* Track-atlas sampler A/B (SELECT+D-RIGHT / dumpreq `bilinear`). Default 0 =
 * GPU_NEAREST, the established point-sampled look. 1 = GPU_LINEAR (bilinear).
 * Applied at alloc and live-reapplied by N3DS_TrackAtlas_ApplyFilter -- the
 * filter lives in the C3D_Tex object and is read at bind, so the flip is a
 * field write with no re-upload, safe mid-race. */
int g_dbg_trackBilinear = 0;

int N3DS_TrackAtlas_Alloc(int unitCount)
{
	char msg[128];
	int i;

	if (unitCount > N3DS_TRACK_ATLAS_MAX_UNITS)
	{
		unitCount = N3DS_TRACK_ATLAS_MAX_UNITS;
	}
	if (unitCount < 0)
	{
		unitCount = 0;
	}

	/* Level reload: free any previous pool before allocating the new one --
	 * same idiom as NativeTrackMesh_Free on the game side. */
	N3DS_TrackAtlas_Free();

	snprintf(msg, sizeof(msg), "[CTR-3DS] linear free before track atlas: %lu KiB (want %d x %d KiB)",
	         (unsigned long)(linearSpaceFree() / 1024), unitCount, (N3DS_TRACK_ATLAS_DIM * N3DS_TRACK_ATLAS_DIM * 2) / 1024);
	N3DS_DebugLog(msg);

	for (i = 0; i < unitCount; i++)
	{
		if (!C3D_TexInit(&s_trackAtlasTextures[i], N3DS_TRACK_ATLAS_DIM, N3DS_TRACK_ATLAS_DIM, GPU_RGBA5551))
		{
			snprintf(msg, sizeof(msg), "[CTR-3DS] track atlas unit %d of %d FAILED to allocate; linear free %lu KiB", i, unitCount,
			         (unsigned long)(linearSpaceFree() / 1024));
			N3DS_DebugLog(msg);
			/* Units [0, i) allocated fine -- keep them live and usable rather
			 * than tearing the whole pool down over a partial failure. */
			s_trackAtlasUnitsAllocated = i;
			return i;
		}
		/* Filter per g_dbg_trackBilinear (above). Bilinear softens near-variant
		 * magnification up close; safe against hard seam bleed because every
		 * packed rect carries >=2 texels of right/bottom pad
		 * (NativeTrackAtlas_Pack), so a 1-texel kernel never reaches a
		 * neighbour's CONTENT; left/top edges fringe one texel toward neighbour
		 * pad, accepted. UVs already address texel centres. */
		{
			GPU_TEXTURE_FILTER_PARAM f = g_dbg_trackBilinear ? GPU_LINEAR : GPU_NEAREST;
			C3D_TexSetFilter(&s_trackAtlasTextures[i], f, f);
		}
		C3D_TexSetWrap(&s_trackAtlasTextures[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

		/* Nearest VIEW of the same texels for the cutout split (see the array
		 * comment). Struct copy shares .data; force its filter to nearest. */
		s_trackAtlasTexturesNearest[i] = s_trackAtlasTextures[i];
		C3D_TexSetFilter(&s_trackAtlasTexturesNearest[i], GPU_NEAREST, GPU_NEAREST);
	}

	s_trackAtlasUnitsAllocated = unitCount;
	return unitCount;
}

/* Re-apply g_dbg_trackBilinear to every live unit. No re-upload -- just the
 * filter field on each C3D_Tex, read at bind -- so this is a valid mid-race
 * A/B flip. No-op when no pool is allocated. */
void N3DS_TrackAtlas_ApplyFilter(void)
{
	GPU_TEXTURE_FILTER_PARAM f = g_dbg_trackBilinear ? GPU_LINEAR : GPU_NEAREST;
	int i;

	for (i = 0; i < s_trackAtlasUnitsAllocated; i++)
	{
		C3D_TexSetFilter(&s_trackAtlasTextures[i], f, f);
	}
}

void N3DS_TrackAtlas_UploadRect(int unit, int dstX, int dstY, int wTexels, int hTexels, const void *tiledRGBA5551)
{
	if ((unit < 0) || (unit >= s_trackAtlasUnitsAllocated))
	{
		return;
	}

	/* Preconditions (see the header comment): dstX/dstY/wTexels/hTexels are
	 * all multiples of 8, and the rect must fit inside the unit's own
	 * N3DS_TRACK_ATLAS_DIM bounds. A violation here is a caller bug, not
	 * routine content variation (the packer's own invariants guarantee this
	 * in practice) -- reject and log it instead of silently writing into the
	 * wrong tile or past the texture's backing memory. */
	if (((dstX & 7) != 0) || ((dstY & 7) != 0) || ((wTexels & 7) != 0) || ((hTexels & 7) != 0) || ((dstX + wTexels) > N3DS_TRACK_ATLAS_DIM) ||
	    ((dstY + hTexels) > N3DS_TRACK_ATLAS_DIM))
	{
		char msg[128];

		snprintf(msg, sizeof(msg), "[CTR-3DS] track atlas upload REJECTED: unit=%d dst=(%d,%d) size=%dx%d (unaligned or out of bounds)", unit, dstX,
		         dstY, wTexels, hTexels);
		N3DS_DebugLog(msg);
		return;
	}

	/* Dividing by 8 here converts to the tile-space units
	 * N3DS_C3DUploadTileBlock actually addresses in. */
	N3DS_C3DUploadTileBlock(&s_trackAtlasTextures[unit], N3DS_TRACK_ATLAS_DIM / 8, dstX / 8, dstY / 8, wTexels / 8, hTexels / 8, tiledRGBA5551);
}

void N3DS_TrackAtlas_Bind(int unit)
{
	if ((unit < 0) || (unit >= s_trackAtlasUnitsAllocated))
	{
		return;
	}

	C3D_TexBind(0, &s_trackAtlasTextures[unit]);
}

/* Bind the same unit's NEAREST view to texture unit 1, for the cutout split.
 * Caller has already validated the unit via N3DS_TrackAtlas_Bind. */
static void N3DS_TrackAtlas_BindNearestView(int unit)
{
	if ((unit < 0) || (unit >= s_trackAtlasUnitsAllocated))
	{
		return;
	}

	C3D_TexBind(1, &s_trackAtlasTexturesNearest[unit]);
}

/* ------------------------------------------------ native track draw --- */
/* See the header comment (n3ds_backend.h) for the bracket contract. */

int N3DS_TrackGeom_Upload(const void *verts, int vertCount, int vertStride, const void *indices, int indexCount)
{
	size_t vertBytes;
	size_t indexOffset;
	size_t totalBytes;
	u8 *block;

	N3DS_TrackGeom_Free();

	if ((verts == NULL) || (vertCount <= 0) || (vertStride <= 0) || (indices == NULL) || (indexCount <= 0))
	{
		return 0;
	}

	vertBytes = (size_t)vertCount * (size_t)vertStride;
	indexOffset = (vertBytes + 7u) & ~(size_t)7u; /* 8-byte align, see the header comment */
	totalBytes = indexOffset + ((size_t)indexCount * sizeof(u16));

	block = (u8 *)linearAlloc(totalBytes);
	if (block == NULL)
	{
		char msg[128];

		snprintf(msg, sizeof(msg), "[CTR-3DS] track geom upload FAILED: %u bytes, linear free %lu KiB", (unsigned)totalBytes,
		         (unsigned long)(linearSpaceFree() / 1024));
		N3DS_DebugLog(msg);
		return 0;
	}

	memcpy(block, verts, vertBytes);
	memcpy(block + indexOffset, indices, (size_t)indexCount * sizeof(u16));

	s_trackGeomMem = block;
	s_trackIndexPtr = block + indexOffset;
	s_trackGeomVertCount = vertCount;
	s_trackGeomVertStride = vertStride;

	/* Rebuilt every upload (not just once): base_paddr depends on THIS
	 * level's linearAlloc address, which N3DS_TrackAtlas_Alloc-style level
	 * reloads change every time. s_trackAttrInfo is NOT rebuilt here -- its
	 * layout is fixed and already built once in N3DS_C3DInit. */
	BufInfo_Init(&s_trackBufInfo);
	BufInfo_Add(&s_trackBufInfo, s_trackGeomMem, vertStride, 3, 0x210);

	s_trackGeomReady = 1;
	return 1;
}

void N3DS_TrackGeom_Free(void)
{
	if (s_trackGeomMem != NULL)
	{
		linearFree(s_trackGeomMem);
		s_trackGeomMem = NULL;
	}
	s_trackIndexPtr = NULL;
	s_trackGeomReady = 0;
	s_trackGeomVertCount = 0;
	s_trackGeomVertStride = 0;
}

/*
 * Rewrite the UV pair of `count` already-uploaded vertices in place.
 *
 * This is how animated track textures (turbo-pad arrows, waterfalls, lava)
 * move at all under the resident renderer -- everything else about a resident
 * vertex is fixed at load, but which frame of a texture chain it samples is
 * not, and the chain advances every vblank.
 *
 * Two things this must get right, both learned the hard way elsewhere in this
 * file:
 *   - The GPU reads PHYSICAL memory and the CPU wrote through the data cache,
 *     so the touched range has to be flushed before the next draw or the GPU
 *     samples the previous frame's UVs (or worse, a mix). Flushed as ONE range
 *     spanning min..max of the batch rather than per vertex: a cache-clean
 *     call has fixed overhead that swamps 4 bytes of payload.
 *   - Every index is bounds-checked against the block's OWN recorded vertex
 *     count. The staging buffer the upload copied from is freed, so this block
 *     is the only copy of the level's geometry that exists.
 *
 * THREAD: called on the walk thread after that walk's frame begin has waited
 * for the previous frame's GPU work (see NativeTrackAnim_PatchFrame's
 * comment), never concurrently with a draw out of this same block.
 */
void N3DS_TrackGeom_PatchUV(const unsigned short *vertIndices, const short *uvPairs, int count)
{
	u8 *base;
	int i;
	int lo = 0, hi = 0, touched = 0;

	if (!s_trackGeomReady || (s_trackGeomMem == NULL) || (vertIndices == NULL) || (uvPairs == NULL) || (count <= 0))
	{
		return;
	}

	base = (u8 *)s_trackGeomMem;

	for (i = 0; i < count; i++)
	{
		int vi = (int)vertIndices[i];
		s16 *uv;

		if ((vi < 0) || (vi >= s_trackGeomVertCount))
		{
			continue;
		}

		/* Offset 6 in N3DS_TrackVertex -- see the struct's field comments in
		 * n3ds_backend.h, which pin every s16 to an even offset for exactly
		 * this kind of direct write. */
		uv = (s16 *)(void *)(base + ((size_t)vi * (size_t)s_trackGeomVertStride) + 6);
		uv[0] = uvPairs[i * 2 + 0];
		uv[1] = uvPairs[i * 2 + 1];

		if (!touched)
		{
			lo = vi;
			hi = vi;
			touched = 1;
		}
		else
		{
			if (vi < lo)
			{
				lo = vi;
			}
			if (vi > hi)
			{
				hi = vi;
			}
		}
	}

	if (touched)
	{
		/* No flush here: widen the frame's patched range and let
		 * N3DS_TrackGeom_FlushPatched issue one flush over the union. */
		N3DS_TrackGeom_MarkPatched(lo, hi);
	}
}

/*
 * The same in-place batched rewrite, for POSITION and COLOR rather than
 * UV -- the animated-vertex case (water surfaces that physically rise and
 * fall, AnimateWater.c writing LevVertex.pos/.color_hi every frame).
 *
 * Separate from PatchUV rather than one call with optional arguments because
 * the two have completely different populations and cadences: UVs change only
 * when a texture chain steps to its next frame (often several vblanks apart,
 * a handful of vertices), positions change every single frame for every
 * animated vertex on the track. Fusing them would make the cheap case pay the
 * expensive case's flush range.
 *
 * xyz is three shorts per entry (offset 0), rgb three bytes (offset 10) -- see
 * N3DS_TrackVertex. Same bounds check and same single range flush as PatchUV,
 * same thread rule.
 */
void N3DS_TrackGeom_PatchPosColor(const unsigned short *vertIndices, const short *xyz, const unsigned char *rgb, int count)
{
	u8 *base;
	int i;
	int lo = 0, hi = 0, touched = 0;

	if (!s_trackGeomReady || (s_trackGeomMem == NULL) || (vertIndices == NULL) || (xyz == NULL) || (rgb == NULL) || (count <= 0))
	{
		return;
	}

	base = (u8 *)s_trackGeomMem;

	for (i = 0; i < count; i++)
	{
		int vi = (int)vertIndices[i];
		u8 *vert;
		s16 *pos;

		if ((vi < 0) || (vi >= s_trackGeomVertCount))
		{
			continue;
		}

		vert = base + ((size_t)vi * (size_t)s_trackGeomVertStride);
		pos = (s16 *)(void *)vert;
		pos[0] = xyz[i * 3 + 0];
		pos[1] = xyz[i * 3 + 1];
		pos[2] = xyz[i * 3 + 2];
		vert[10] = rgb[i * 3 + 0];
		vert[11] = rgb[i * 3 + 1];
		vert[12] = rgb[i * 3 + 2];

		if (!touched)
		{
			lo = vi;
			hi = vi;
			touched = 1;
		}
		else
		{
			if (vi < lo)
			{
				lo = vi;
			}
			if (vi > hi)
			{
				hi = vi;
			}
		}
	}

	if (touched)
	{
		/* No flush here: widen the frame's patched range and let
		 * N3DS_TrackGeom_FlushPatched issue one flush over the union. */
		N3DS_TrackGeom_MarkPatched(lo, hi);
	}
}

void N3DS_TrackDraw_Begin(const float *qx, const float *qy, const float *qw, float h)
{
	C3D_TexEnv *env;

	/* Stereo journal: recorded at the TOP, mirroring the call itself rather
	 * than its effect -- the replay re-invokes this function, whose own
	 * guards and global reads (geomReady, selTrueDepth, depthTest state)
	 * re-evaluate identically within the same frame. Same pattern for every
	 * recorded call below. */
	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_TRACK_BEGIN);

		if (op != NULL)
		{
			memcpy(op->u.mtx.qx, qx, sizeof(op->u.mtx.qx));
			memcpy(op->u.mtx.qy, qy, sizeof(op->u.mtx.qy));
			memcpy(op->u.mtx.qw, qw, sizeof(op->u.mtx.qw));
			op->u.mtx.h = h;
		}
	}

	s_trackDrawActive = 0;

	if (!s_trackGeomReady)
	{
		return;
	}

	C3D_BindProgram(&s_trackShaderProgram);
	C3D_SetAttrInfo(&s_trackAttrInfo);
	C3D_SetBufInfo(&s_trackBufInfo);

	/*
	 * Backface culling, scoped to the TRACK only. Retail rejects back-facing
	 * SINGLE-SIDED faces per-face (GTE NCLIP winding test gated by the
	 * DOUBLE_SIDED bit, game/226 DrawLevelOvr); the bake emits every face
	 * regardless of winding and the GPU runs GPU_CULL_NONE globally, so PICA
	 * hardware culling stands in for it (it removes the Crash Cove second
	 * waterfall and the like). Safe because the bake gives triangles and
	 * double-sided faces a reverse-wound copy (native_track_mesh.c), so only
	 * single-sided quads are actually culled. Restored to NONE in
	 * N3DS_TrackDraw_End so the packet/UI/model draws after the track are never
	 * affected.
	 */
	/* A mirrored world reverses every triangle's screen winding (native_track_frame.c), so the
	 * faces kept are the clockwise ones. */
	C3D_CullFace(s_mirrorWorld ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);

	/* State 2 (TRACKRO, see g_dbg_depthTest): the track still TESTS against
	 * depth -- so it can still be hidden by nearer packet content, which is
	 * what makes this a fair test rather than a "draw the track first" hack --
	 * but stops WRITING depth, so nothing the track draws can outrank a kart.
	 * Restored in N3DS_TrackDraw_End rather than left set: the merge flush
	 * immediately after assumes the frame-wide pair from N3DS_C3DBeginFrame. */
	if (g_dbg_depthTest == 2)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_COLOR);
	}

	/*
	 * Assert opaque blending explicitly rather than
	 * inheriting whatever the LAST packet draw left set. N3DS_C3DSetBlendMode
	 * memoizes through a static `lastMode` (below in this file) and this
	 * function never called it before, so track draws silently picked up
	 * additive/subtractive blend state on any frame containing sparks,
	 * shadows, or transparency, and plain opaque on frames without -- an
	 * intermittent, content-dependent bug that is exactly the worst kind for
	 * an A/B fps measurement (same toggle setting, different blend state,
	 * different-looking or differently-costed frame depending on what else
	 * was on screen a moment ago). Mode 0 is BM_NONE/opaque, matching this
	 * pass's actual content (untextured vertex-lit track geometry, rung 3a --
	 * see N3DS_TrackDraw_Begin's own header comment; rung 3b's textured
	 * draws stay opaque too, design 4.4's exclusions keep semi-trans faces on
	 * the packet path). No restore in N3DS_TrackDraw_End is needed for this:
	 * every packet emit re-asserts its OWN mode through this same memoized
	 * setter before it draws anything, so whatever N3DS_TrackDraw_Begin set
	 * here is always overwritten before it could affect packet content --
	 * don't "fix" this by adding a redundant restore.
	 */
	N3DS_C3DSetBlendMode(0);

	/* Depth writes are ON entering the bracket (N3DS_C3DBeginFrame's frame-wide
	 * pair, or state 2's read-only variant just above). The blend pass turns
	 * them off for its own draws and this memo tracks that; -1 rather than 1 so
	 * the first draw of the frame asserts explicitly instead of trusting an
	 * inherited state that a future change to BeginFrame could alter. */
	s_trackDepthWriteState = -1;

	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocQx, qx[0], qx[1], qx[2], qx[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocQy, qy[0], qy[1], qy[2], qy[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocQw, qw[0], qw[1], qw[2], qw[3]);
	{
		/* hParam.y/z select the track's depth axis, and they are driven by the
		 * SAME toggle that makes packets stamp sz3 (g_dbg_packetDepth,
		 * native_gpu.c). One switch on purpose: with only one side flipped the
		 * two geometry sources sit on different axes and the result is worse
		 * than either alone, which is exactly the sort of half-state that
		 * costs an owner hardware sitting to diagnose.
		 *
		 * z is supplied as 1-y so the shader needs no subtraction (PICA200 has
		 * no SUB opcode). Capture-off builds pin this to the ordinal path, so
		 * they behave exactly as they do today. */
		float selTrueDepth = 0.0f;

		{
			extern int g_dbg_packetDepth;

			selTrueDepth = (g_dbg_packetDepth != 0) ? 1.0f : 0.0f;
		}

		/* hParam.w: drawOrder tie-break weight, in view.z units per drawOrder
		 * step (see N3DS_TRACK_DRAWORDER_WEIGHT). */
		C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocH, h, selTrueDepth, 1.0f - selTrueDepth, N3DS_TRACK_DRAWORDER_WEIGHT);

		/* Prove the selector actually REACHES the shader. Every depth number
		 * logged so far is CPU-side arithmetic showing what the value would be,
		 * which is not evidence the GPU used it -- and "the fix changed almost
		 * nothing visible" is exactly what an unbound uniform looks like. uLoc
		 * is included because -1 means shaderInstanceGetUniformLocation never
		 * found it, in which case every C3D_FVUnifSet above has been writing
		 * into nowhere. */
		{
			static float s_lastSel = -1.0f;

			if (s_lastSel != selTrueDepth)
			{
				char msg[96];

				snprintf(msg, sizeof(msg), "[CTR-3DS] track depth sel=%d uLocH=%d uLocDepth=%d", (int)selTrueDepth, s_trackULocH, s_trackULocDepthRange);
				N3DS_DebugLog(msg);
				s_lastSel = selTrueDepth;
			}
		}
	}
	/* SAME ortho matrix the packet path uses (n3ds_backend.c's s_projection,
	 * computed by N3DS_C3DSetProjection) -- this is what puts track content
	 * and packet content on one shared depth axis (see the shader's own
	 * header comment for the row3/outpos.w reasoning this depends on). */
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_trackULocProjection, &s_projection);

	/* Current eye's parallax (zero in mono). Per bracket, not per frame:
	 * the right-eye replay re-enters this function with s_stereoEye = 1. */
	N3DS_StereoUploadUniform(s_trackULocStereo, (const float(*)[4])s_stereoVecWorld);

	/*
	 * Vertex-color-only output: 3a is untextured (color_hi is already baked
	 * per vertex -- the familiar "textureless mode" look). Deliberately NOT
	 * a rebind of the packet path's own reserved white decode-cache cell
	 * (N3DS_MERGE_UNTEX_BIN/N3DS_WHITE_ATLAS, native_renderer_3ds.c) even
	 * though that is this renderer's existing precedent for "untextured":
	 * that cell is addressed by a plain atlas-index constant defined in a
	 * DIFFERENT translation unit (the unity build), and hardcoding the same
	 * index here would be an unenforced cross-TU coupling this project's own
	 * TU/ABI boundary rule argues against -- if that index ever changed on
	 * the unity side, this file would silently sample the wrong atlas with
	 * no compile-time signal. GPU_REPLACE from GPU_PRIMARY_COLOR reaches the
	 * identical pixel result (texture0 is never sampled at all) without that
	 * coupling. N3DS_TrackDraw_End restores GPU_MODULATE.
	 */
	env = C3D_GetTexEnv(0);
	C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
	C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	C3D_DirtyTexEnv(env);

	/* No PS1 2x "bright" modulate for untextured Gouraud content -- matches
	 * N3DS_MERGE_UNTEX_BIN's own choice of doubling=0 for the packet path's
	 * untextured bin. Rung 3b's textured draws are where the PS1 2x texel*
	 * color modulate belongs (design 12b), not here. */
	N3DS_C3DSetColorDoubling(0);

	/* The packet path owns texenv 0 between track frames, so whatever
	 * N3DS_TrackDraw_SetTextured last recorded is stale by now. */
	s_trackTexturedState = 0;

	s_trackDrawActive = 1;
}

/*
 * Rung 3b: flip the track program's combiner between textured and untextured.
 * Memoized -- a track frame issues one draw per (leaf x atlas unit) and most
 * consecutive draws share a state, so re-emitting the env every draw would be
 * pure command-buffer traffic on the platform this campaign exists to speed up.
 * The memo is reset by N3DS_TrackDraw_Begin, since the packet path owns texenv
 * 0 between frames and will have changed it.
 *
 * Colour doubling is part of the state, not incidental: PS1 texel*colour
 * modulate is 2x (design 12b), which is exactly why rung 3a deliberately left
 * doubling off for its untextured draws. Textured track content needs it back
 * or every texture reads half-brightness against retail.
 */
/*
 * mode: 0 = untextured (flat Gouraud), 1 = textured single-sample (one unit
 * feeds both channels), 2 = textured CUTOUT SPLIT (colour from the smooth
 * bilinear unit 0, alpha from the sharp nearest unit 1 -- crisp cutout, no
 * bilinear rim, smooth texels; docs approach #2). Mode 2 is used only for
 * opaque draws while bilinear is on; the blend pass stays mode 1 so its soft
 * alpha edges (the Labs glow, waterfalls) are untouched.
 */
static void N3DS_TrackDraw_SetTextured(int mode)
{
	C3D_TexEnv *env;

	if (s_trackTexturedState == mode)
	{
		return;
	}

	env = C3D_GetTexEnv(0);
	if (mode == 0)
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	else if (mode == 2)
	{
		/* RGB keeps the PS1 2x texel*colour modulate (unit 0, bilinear). Alpha
		 * takes the crisp mask from unit 1 (nearest), still modulated by the
		 * vertex alpha exactly as the single path did. C3D_RGB and C3D_Alpha are
		 * independent env slots, so these four calls fully define the stage. */
		C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
		C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	}
	C3D_DirtyTexEnv(env);
	N3DS_C3DSetColorDoubling((mode != 0) ? 1 : 0);
	s_trackTexturedState = mode;
}

void N3DS_TrackDraw_Leaf(int firstIndex, int indexCount, float otIndexLo, float ordinalLo, float ordinalSpan, float invIndexSpan, int atlasBin, int blend, int uvDx, int uvDy)
{
	const u16 *indexBase;

	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_TRACK_LEAF);

		if (op != NULL)
		{
			op->u.leaf.firstIndex = firstIndex;
			op->u.leaf.indexCount = indexCount;
			op->u.leaf.otIndexLo = otIndexLo;
			op->u.leaf.ordinalLo = ordinalLo;
			op->u.leaf.ordinalSpan = ordinalSpan;
			op->u.leaf.invIndexSpan = invIndexSpan;
			op->u.leaf.atlasBin = atlasBin;
			op->u.leaf.blend = blend;
			op->u.leaf.uvDx = uvDx;
			op->u.leaf.uvDy = uvDy;
		}
	}

	if (!s_trackDrawActive || (indexCount <= 0) || (firstIndex < 0))
	{
		return;
	}

	/*
	 * Rung 3b: one draw binds one texture, which is the entire reason the mesh
	 * builder splits a leaf's indices per atlas unit. A bin at or past the
	 * allocated unit count means "no placed texture for these faces" -- draw
	 * them untextured rather than binding a stale unit and sampling another
	 * rect's texels, which would look like garbage welded onto the track.
	 *
	 * Texture enable is a TEXTURE-COMBINER decision, not a vertex-shader one:
	 * the vertex program cannot branch cheaply, so it always computes the atlas
	 * UV and N3DS_TrackDraw_SetTextured picks the combiner setup that either
	 * samples it or ignores it.
	 *
	 * The selector does not ride hParam.w: that is the drawOrder tie-break
	 * weight. On a part whose uniform names are offsets into ONE shared
	 * physical register file, a wrong belief about who owns a component is
	 * exactly how a past projection collision happened, so the transparency
	 * path's uvOffset is its own uniform rather than parked on a spare
	 * component.
	 */
	if ((atlasBin >= 0) && (atlasBin < s_trackAtlasUnitsAllocated))
	{
		/* Cutout split (smooth colour, crisp alpha) only for OPAQUE draws while
		 * bilinear is on. blend != 3 is the blend pass -- its soft alpha edges
		 * are the look we want to keep, so it stays single-sample (mode 1). With
		 * bilinear off, mode 1 is the nearest single-sample path unchanged. */
		int split = g_dbg_trackBilinear && (blend == 3);

		N3DS_TrackAtlas_Bind(atlasBin);
		if (split)
		{
			N3DS_TrackAtlas_BindNearestView(atlasBin);
		}
		N3DS_TrackDraw_SetTextured(split ? 2 : 1);
	}
	else
	{
		N3DS_TrackDraw_SetTextured(0);
	}

	/*
	 * Negate otIndexLo here, not on the caller's side: PICA200 has no SUB
	 * opcode (verified against picasso's own mnemonic list -- see the
	 * shader's header comment), so the shader computes "otIndex_v -
	 * otIndexLo" as an ADD against a pre-negated constant. Keeping that
	 * encoding choice inside this GPU-facing function, rather than pushing a
	 * pre-negated otIndexLo out through the plain-scalar API, is what keeps
	 * platform/native_track_frame.c's math a direct, literal reading of the
	 * design doc's formulas.
	 */
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocDepthRange, -otIndexLo, ordinalLo, ordinalSpan, invIndexSpan);

	/*
	 * SEMI-TRANSPARENCY.
	 *
	 * blend is the rect's PSX ABR mode for a range the blend pass owns, or 3
	 * (TRACK_ABR_OPAQUE) for ordinary content. uvDx/uvDy are the TEXEL delta to
	 * the rect's PASS2 placement; the shader wants them in its own encoded units,
	 * which are 2x fixed point, so the doubling happens here -- next to the
	 * comment that says why -- rather than in the mesh builder, which thinks in
	 * texels throughout. V is negated because the baked UV encodes V flipped for
	 * PICA's bottom-up T axis (NativeTrackAtlas_EncodeUV: outV = (2*DIM-1) -
	 * (atlasV<<1)), so a downward atlas shift must SUBTRACT in encoded space --
	 * the shader adds uvOffset to the flipped UV, so the sign has to invert here.
	 * Silent whenever PASS2 packs on the primary's shelf row (uvDy == 0).
	 *
	 * Written on EVERY draw including opaque ones. An unset uniform component
	 * holds whatever the last program to use that physical register left there,
	 * and PICA uniforms are shared physical registers across programs, not
	 * per-program handles -- a past projection collision was exactly that
	 * mistake and it destroyed every packet drawn after a track draw.
	 */
	/* .z/.w unused (was the behind-eye depth-kick lever, removed); written
	 * every draw to avoid the stale-register issue the paragraph above
	 * describes. */
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_trackULocUvOffset, (float)(uvDx * 2), (float)(-uvDy * 2), 0.0f, 0.0f);

	/*
	 * Blended ranges draw with their own PS1 blend mode and, critically, with
	 * depth WRITES OFF. Test stays on, so blended geometry is still correctly
	 * hidden by opaque things in front of it; but it must not write, or two
	 * blended surfaces would occlude each other by depth instead of compositing,
	 * and a waterfall would punch a hole in whatever is behind it.
	 *
	 * The caller guarantees every opaque range is issued before any blended one
	 * (platform/native_track_frame.c runs two passes), so this pair of state
	 * changes happens at most twice per frame, not per draw. Memoized anyway
	 * because "at most twice" is a property of the caller, and a future caller
	 * that interleaved would otherwise pay a GPU state change per draw silently.
	 */
	{
		const int wantDepthWrite = (blend == 3) ? 1 : 0;

		if (s_trackDepthWriteState != wantDepthWrite)
		{
			s_trackDepthWriteState = wantDepthWrite;
			/* Leave state 2 (TRACKRO) alone -- it already means "test but never
			 * write" for the whole track, which is a superset of what the blend
			 * pass needs, and stomping it here would silently disable the
			 * isolation diagnostic. */
			if (g_dbg_depthTest != 2)
			{
				C3D_DepthTest(true, GPU_LEQUAL, wantDepthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
			}
		}

		/* ABR -> BlendMode is +1, and that is not a guess: native_gpu.c:49 spells
		 * the packet path's own conversion as
		 *   #define GET_TPAGE_BLEND(tpage) ((BlendMode)(((tpage >> 5) & 3) + 1))
		 * -- the same field, the same arithmetic. The enum
		 * (include/platform/native_renderer_types.h) lines up term for term:
		 * BM_NONE=0, BM_AVERAGE=1 (ABR0, B/2+F/2), BM_ADD=2 (ABR1, B+F),
		 * BM_SUBTRACT=3 (ABR2, B-F), BM_ADD_QUATER_SOURCE=4 (ABR3). Track
		 * content and packet content therefore reach identical GPU blend state
		 * from identical source data, which is the only way the two can agree on
		 * a waterfall drawn half by each. */
		N3DS_C3DSetBlendMode((blend == 3) ? 0 : (blend + 1));
	}

	indexBase = (const u16 *)s_trackIndexPtr + firstIndex;
	C3D_DrawElements(GPU_TRIANGLES, indexCount, C3D_UNSIGNED_SHORT, indexBase);
}

void N3DS_TrackDraw_End(void)
{
	N3DS_StereoRecord(N3DS_STEREO_OP_TRACK_END);

	/* Program + attribute layout + vertex buffer + texture-modulate combiner
	 * -- N3DS_BindPacketDrawState's own body covers all four (it is the same
	 * function N3DS_C3DInit uses to build this state the first time), which
	 * is the full list of GPU state N3DS_TrackDraw_Begin changed away from
	 * the packet path's own shape. Unconditional: even if Begin no-op'd
	 * (geometry not ready, s_trackDrawActive never went true), restoring
	 * packet state here is always safe and cheap, and guarantees the packet
	 * path's merge-bin flush right after this call is never left pointed at
	 * track state by a call-order mistake upstream. */
	N3DS_BindPacketDrawState();

	/* Undo the track-scoped backface cull so the packet/UI/model/sky draws
	 * after the track are never culled (their winding is not guaranteed).
	 * Matches the global GPU_CULL_NONE set once in N3DS_C3DInit. */
	C3D_CullFace(GPU_CULL_NONE);

	/* Undo state 2's read-only depth mask. Unconditional on the state rather
	 * than mirrored off a flag set in Begin: Begin can return early (geometry
	 * not ready) having changed nothing, and re-asserting the frame-wide pair
	 * is idempotent, so this cannot desync the way a paired flag could. */
	if (g_dbg_depthTest == 2)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
	else if (s_trackDepthWriteState == 0)
	{
		/* The blend pass turned depth writes off for its own draws. Restore the
		 * frame-wide pair before the packet path's merge flush runs, or every
		 * packet drawn after the track this frame would silently stop writing
		 * depth -- karts would then fail to occlude each other, on exactly the
		 * frames that happened to contain a waterfall. Conditioned on the memo
		 * rather than run unconditionally so a frame with no blended content
		 * issues no redundant state change at all. */
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}

	s_trackDepthWriteState = -1;
	s_trackDrawActive = 0;
}


/*
 * Rung 2 model draw bracket. Mirrors N3DS_TrackDraw_* in shape; the differences
 * are all consequences of models being per-frame dynamic rather than resident.
 */
void N3DS_ModelDraw_Begin(void)
{
	/*
	 * Start from the packet path's own state, then swap only the program.
	 * BindPacketDrawState installs the attribute layout AND the buffer binding
	 * model vertices were appended into (N3DS_C3DAppendVertices writes the
	 * shared per-frame buffer), so this is not a lazy shortcut -- it is the
	 * only way to be sure the layout matches what wrote the data.
	 */
	N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_BEGIN);

	N3DS_BindPacketDrawState();
	C3D_BindProgram(&s_modelShaderProgram);

	/*
	 * The SAME ortho matrix the packet path uses. The model shader declares
	 * projection[4] first precisely so it occupies the registers ctr.v.pica
	 * expects, but "the registers hold the right value" is a separate claim
	 * from "the registers are where we think" -- upload it explicitly rather
	 * than inheriting whatever the last bound program left, which is the class
	 * of assumption that produced a past uniform-collision bug.
	 */
	C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_modelULocProjection, &s_projection);

	/* Current eye's parallax (zero in mono). The model program's `stereo`
	 * register (f8) is ctr_track's depthRange, clobbered every track leaf --
	 * this bracket always opens after the track bracket closed, so this
	 * per-bracket upload is exactly what makes that safe. */
	N3DS_StereoUploadUniform(s_modelULocStereo, (const float(*)[4])s_stereoVecWorld);

	/*
	 * Assert opaque blending, for the reason spelled out at length in
	 * N3DS_TrackDraw_Begin: N3DS_C3DSetBlendMode memoizes, so without this the
	 * model pass silently inherits additive or subtractive state from whatever
	 * packet content happened to draw last -- an intermittent, content-
	 * dependent difference, which is the worst kind to meet during an fps A/B.
	 */
	N3DS_C3DSetBlendMode(0);

	/* The packet path owns texenv 0 and the atlas binding between model
	 * frames, so both memos are stale by now. */
	s_modelTexturedState = -1;
	s_modelBoundAtlas = -1;
	s_modelDepthWriteState = -1;
	s_modelDrawActive = 1;
	s_modelSkyDome = 0;
}

static void N3DS_ModelDraw_InstanceImpl(const float *qx, const float *qy, const float *qw, float h, int skyDome)
{
	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_INSTANCE);

		if (op != NULL)
		{
			memcpy(op->u.mtx.qx, qx, sizeof(op->u.mtx.qx));
			memcpy(op->u.mtx.qy, qy, sizeof(op->u.mtx.qy));
			memcpy(op->u.mtx.qw, qw, sizeof(op->u.mtx.qw));
			op->u.mtx.h = h;
			op->u.mtx.skyDome = (u8)(skyDome != 0);
		}
	}

	if (!s_modelDrawActive)
	{
		return;
	}

	s_modelSkyDome = (skyDome != 0);

	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocQx, qx[0], qx[1], qx[2], qx[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocQy, qy[0], qy[1], qy[2], qy[3]);
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocQw, qw[0], qw[1], qw[2], qw[3]);

	/* x = H, y = sky-dome select; z/w are unused but written every draw,
	 * because an unset uniform component holds whatever the last program that
	 * used this register put there. */
	C3D_FVUnifSet(GPU_VERTEX_SHADER, s_modelULocH, h, s_modelSkyDome ? 1.0f : 0.0f, 0.0f, 0.0f);
}

void N3DS_ModelDraw_Instance(const float *qx, const float *qy, const float *qw, float h)
{
	N3DS_ModelDraw_InstanceImpl(qx, qy, qw, h, 0);
}

void N3DS_ModelDraw_InstanceSkyDome(const float *qx, const float *qy, const float *qw, float h)
{
	N3DS_ModelDraw_InstanceImpl(qx, qy, qw, h, 1);
}

/*
 * Textured/untextured combiner, memoized. 35% of a real frame's model triangles
 * are untextured (measured, design section 7), so both states are hot and the
 * memo earns its keep.
 *
 * Colour doubling is part of the state: PS1 texel*colour modulate is 2x, which
 * is why the untextured leg turns it off -- the same pairing N3DS_TrackDraw_
 * SetTextured makes, and for the same reason (without it every model texture
 * reads half-brightness against retail).
 */
static void N3DS_ModelDraw_SetTextured(int textured)
{
	C3D_TexEnv *env;

	if (s_modelTexturedState == textured)
	{
		return;
	}

	env = C3D_GetTexEnv(0);
	if (textured)
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
	}
	else
	{
		C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
		C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
	}
	C3D_DirtyTexEnv(env);
	N3DS_C3DSetColorDoubling(textured ? 1 : 0);
	s_modelTexturedState = textured;
}

void N3DS_ModelDraw_Run(int startVertex, int triangleCount, int atlasIndex, int textured, int blend)
{
	{
		N3DS_StereoOp *op = N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_RUN);

		if (op != NULL)
		{
			op->u.mrun.startVertex = startVertex;
			op->u.mrun.triCount = triangleCount;
			op->u.mrun.atlas = atlasIndex;
			op->u.mrun.textured = textured;
			op->u.mrun.blend = blend;
		}
	}

	if (!s_modelDrawActive || (triangleCount <= 0) || (startVertex < 0))
	{
		return;
	}

	/*
	 * Semi-transparency. blend is a BlendMode:
	 * 0 = opaque, 1..4 the PSX modes. A blended run is the PASS2 half of a
	 * semi-transparent face and must not WRITE depth -- its PASS1 half already
	 * wrote it in pass 0, and letting the blended half write again would make two
	 * overlapping translucent surfaces occlude rather than composite.
	 *
	 * The caller issues every opaque run before any blended one, so this pair of
	 * state changes happens at most twice per frame. Memoized regardless, because
	 * "at most twice" is a property of the caller and a future interleaving one
	 * would otherwise pay a GPU state change per run without anyone noticing.
	 */
	{
		const int wantDepthWrite = ((blend == 0) && !s_modelSkyDome) ? 1 : 0;

		if (s_modelDepthWriteState != wantDepthWrite)
		{
			s_modelDepthWriteState = wantDepthWrite;
			C3D_DepthTest(true, GPU_LEQUAL, wantDepthWrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
		}

		N3DS_C3DSetBlendMode(blend);
	}

	if (textured && (atlasIndex >= 0) && (atlasIndex < N3DS_ATLAS_COUNT))
	{
		/* Bind only on change. The caller sorts a frame's triangles into
		 * texture-homogeneous runs, so consecutive runs frequently share an
		 * atlas across instances and this skips most of the binds. */
		if (s_modelBoundAtlas != atlasIndex)
		{
			N3DS_C3DBindAtlas(atlasIndex);
			s_modelBoundAtlas = atlasIndex;
		}
		N3DS_ModelDraw_SetTextured(1);
	}
	else
	{
		/* Out-of-range atlas draws untextured rather than binding a stale unit
		 * and sampling another texture's texels, which would read as garbage
		 * welded onto a kart. Same rule N3DS_TrackDraw_Leaf applies. */
		N3DS_ModelDraw_SetTextured(0);
	}

	C3D_DrawArrays(GPU_TRIANGLES, startVertex, triangleCount * 3);
}

void N3DS_ModelDraw_End(void)
{
	N3DS_StereoRecord(N3DS_STEREO_OP_MODEL_END);

	/* Restores program, attribute layout, buffer binding and the modulate
	 * combiner -- the full set this bracket touched. Unconditional for the
	 * same reason N3DS_TrackDraw_End is: cheap, idempotent, and it cannot
	 * desync if Begin returned early. */
	N3DS_BindPacketDrawState();

	/* The blend pass turned depth writes off. Restore the frame-wide pair before
	 * anything else draws, or every packet issued after the models this frame
	 * would silently stop writing depth -- and only on frames that happened to
	 * contain a particle or a shadow, which is the intermittent, content-
	 * dependent kind of bug that costs a hardware sitting to find. */
	if (s_modelDepthWriteState == 0)
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}

	s_modelDrawActive = 0;
	s_modelTexturedState = -1;
	s_modelBoundAtlas = -1;
	s_modelDepthWriteState = -1;
	s_modelSkyDome = 0;
}

/*
 * PS1 semi-transparency (the four ABR modes) via PICA's fixed-function blend
 * unit -- the same equation/factor mapping PC uses in GL
 * (NativeRenderer_SetBlendMode, native_renderer.c), including its constant-
 * color trick: one blend color of RGB=0.25 / A=0.5 (set once in N3DS_C3DInit)
 * serves both AVERAGE (constant ALPHA carries the 1/2) and ADD_QUARTER
 * (constant COLOR carries the 1/4).
 *
 * BM_NONE is blending "enabled" with ONE/ZERO rather than a switch to
 * GPU logic-op mode: numerically identical to opaque, and it keeps the
 * fragment pipeline in one mode instead of toggling GPUREG_COLOR_OPERATION
 * per split.
 *
 * Deduped against the last-applied mode: C3D state persists across frames,
 * so a redundant call would only dirty state for nothing. citro3d records a
 * state change into the command list at the NEXT draw, and the whole frame
 * is one deferred command list, so calling this between draws mid-frame
 * takes effect exactly at the following draw -- same ordering contract as
 * every other state setter here.
 *
 * Alpha channel stays ONE/ZERO in every mode (PC does the same via
 * glBlendFuncSeparate): the framebuffer alpha carries nothing this renderer
 * reads, and blending it with the RGB factors could only corrupt it.
 */
/* ------------------------------------------------ bottom-screen minimap --- */
/*
 * Second-screen pass for the in-race minimap -- see the header's contract
 * comment. Same non-fatal target-allocation policy as the stereo right eye:
 * a console that cannot allocate it keeps the map on the top screen and says
 * so once.
 *
 * The transfer's OUT format must match the framebuffer format the LCD is
 * actually scanning, queried at runtime -- NOT assumed. consoleInit owns the
 * bottom screen's format and libctru is free to pick a 16-bit one; a
 * bytes-per-pixel mismatch between what the transfer writes and what the LCD
 * reads shows as the map interlaced with itself in vertical stripes, 2-3
 * ghost copies down the panel (observed live on the first Azahar sitting,
 * with an RGB8-out transfer assumed from a stale comment).
 *
 * While this pass owns the screen the debug console must not paint: console
 * text lands in the SAME single-buffered framebuffer the display transfer
 * fills, so a row repainted between transfers would scribble over the map.
 * s_bottomMapOwnFrames is a short present-side TTL rather than an exact
 * handshake because present and the render frame run on different threads;
 * it self-heals back to the console within a few presents of the map going
 * quiet (menus, race end, 'btmmap' off).
 */
static C3D_RenderTarget *s_renderTargetBottom = NULL;
static int s_bottomTargetFailed = 0;
static C3D_Mtx s_bottomSavedProjection;
/* s_bottomMapOwnFrames (the present-side TTL) is declared beside
 * N3DS_GfxPresent, its consumer. */

static u32 N3DS_BottomTransferFlags(void)
{
	const GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(GFX_BOTTOM);
	u32 out;

	switch (fmt)
	{
	case GSP_RGBA8_OES:
	{
		out = GX_TRANSFER_FMT_RGBA8;
		break;
	}
	case GSP_BGR8_OES:
	{
		out = GX_TRANSFER_FMT_RGB8;
		break;
	}
	case GSP_RGB565_OES:
	{
		out = GX_TRANSFER_FMT_RGB565;
		break;
	}
	case GSP_RGB5_A1_OES:
	{
		out = GX_TRANSFER_FMT_RGB5A1;
		break;
	}
	case GSP_RGBA4_OES:
	default:
	{
		out = GX_TRANSFER_FMT_RGBA4;
		break;
	}
	}
	{
		char msg[96];

		snprintf(msg, sizeof(msg), "[CTR-3DS] btmmap: bottom fb format=%d -> transfer out fmt=%d", (int)fmt, (int)out);
		N3DS_DebugLog(msg);
	}
	return GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
	       GX_TRANSFER_OUT_FORMAT(out) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
}

/* Nonzero while the map pass holds the bottom panel -- the stats drip
 * (N3DS_StatsDripRow, renderer TU) must not paint console rows over it;
 * suppressing the flush/swap in N3DS_GfxPresent alone is not enough, because
 * console glyphs are written into the shared framebuffer at print time. */
int N3DS_BottomMapOwnsScreen(void)
{
	return s_bottomMapOwnFrames > 0;
}

int N3DS_BottomMapDraw_Begin(float left, float right, float top, float bottom)
{
	if (s_renderTargetBottom == NULL)
	{
		if (s_bottomTargetFailed)
		{
			return 0;
		}
		s_renderTargetBottom = C3D_RenderTargetCreate(240, 320, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
		if (s_renderTargetBottom == NULL)
		{
			s_bottomTargetFailed = 1;
			N3DS_DebugLog("[CTR-3DS] btmmap: bottom render target FAILED to allocate, minimap stays on top");
			return 0;
		}
		C3D_RenderTargetSetOutput(s_renderTargetBottom, GFX_BOTTOM, GFX_LEFT, N3DS_BottomTransferFlags());
	}

	/* Queued clear before any bottom draw; every draw this frame executes at
	 * FrameEnd's command list, after this fill -- the same ordering
	 * N3DS_C3DBeginFrame's own clears rely on. Depth clears to far
	 * (LEQUAL + clear-to-max polarity, see BeginFrame). */
	C3D_RenderTargetClear(s_renderTargetBottom, C3D_CLEAR_ALL, 0x000000FF, 0x00FFFFFF);
	C3D_FrameDrawOn(s_renderTargetBottom);
	/* Viewport does not follow the framebuffer binding (same note as the right
	 * eye); full 320x240 panel, stored sideways like every 3DS target. */
	C3D_SetViewport(0, 0, 240, 320);
	/* Frame-wide depth pair, mirroring BeginFrame's policy -- whatever the
	 * right-eye replay's last draw left behind is not it. */
	if (g_dbg_depthTest == 1)
	{
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	}
	else
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
	/* Packet program/layout/buffer/combiner + projection + parallax re-upload.
	 * The parallax vector is whichever eye ran last -- irrelevant here: every
	 * routed primitive carries a UI-band stamp, which the shader's band mask
	 * flattens to zero shift. */
	N3DS_BindPacketDrawState();
	/* The display window: same ortho construction as N3DS_C3DSetProjection but
	 * with a non-zero top and NO widescreen extension -- the window is fitted
	 * to the map's own rect, aspect-corrected by the caller. Saved/restored
	 * around the pass because BeginFrame re-uploads the STORED matrix every
	 * frame while SetProjection only rebuilds it on a display-mode change. */
	Mtx_Copy(&s_bottomSavedProjection, &s_projection);
	Mtx_OrthoTilt(&s_projection, left, right, bottom, top, -32769.0f, 32769.0f, true);
	if (s_uLocProjection >= 0)
	{
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	}

	s_bottomMapOwnFrames = 3;
	return 1;
}

/* Re-aim the ortho window mid-pass (the FPS overlay draws through a smaller,
 * non-magnifying window after the map's). Only valid between Begin and End;
 * Begin already saved the real projection and End restores it. */
void N3DS_BottomMapDraw_SetWindow(float left, float right, float top, float bottom)
{
	Mtx_OrthoTilt(&s_projection, left, right, bottom, top, -32769.0f, 32769.0f, true);
	if (s_uLocProjection >= 0)
	{
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	}
}

void N3DS_BottomMapDraw_End(void)
{
	Mtx_Copy(&s_projection, &s_bottomSavedProjection);
	if (s_uLocProjection >= 0)
	{
		C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, s_uLocProjection, &s_projection);
	}
}

/*
 * The right eye: replay the frame's journal against the right target with the
 * right eye's parallax vectors. Called by NativeRenderer_EndScene after every
 * left-eye draw (including the debug atlas inspector, which deliberately
 * stays left-eye-only) and immediately before N3DS_C3DEndFrame submits the
 * whole two-eye command list. No-op in mono.
 *
 * Track/model ops re-enter the same public functions, whose own guards,
 * memos and global reads re-evaluate identically within the frame -- the
 * only thing that differs is s_stereoEye, which their Begin uploads read.
 * Packet/VRAM ops re-issue resolved state inline: doubling and depth-write
 * were resolved at record time (policy ran once, on the left eye), so the
 * replay is policy-free. Both are draws from (baseVertex, count) into the
 * SAME persistent frame VBO the left eye drew from -- nothing is
 * re-appended, so the vertex budget is untouched.
 *
 * On journal overflow the tail is dropped from the RIGHT eye only, loudly;
 * the defensive re-assert at the bottom covers a prefix that ends inside a
 * track/model bracket.
 */
/*
 * Right eye from the left eye's command list.
 *
 * The left eye's draws, from the end of N3DS_C3DBeginFrame to the right-eye
 * point, are one span of the citro3d command buffer. The right eye's draws
 * differ from them only in the render target and the parallax uniforms, so
 * instead of replaying the journal (every draw issued again through citro3d)
 * the span is appended a second time with those words rewritten:
 *   - colour and depth buffer address writes: left target's -> right target's;
 *   - float-uniform data for f4 (`stereo`: the world or packet vector,
 *     whichever program wrote it) and f5 (`stereoUi`). Both are exact sign
 *     rearrangements of the left eye's vector (N3DS_StereoEyeVec):
 *     f4 (x, y, z, w) -> (-x, -y, -w, -z), f5 (x, y, z, w) -> (-x, y, z, w).
 * The copy starts from the GPU state the left eye ended on, so the span must
 * not inherit state from before it: N3DS_StereoCopyOpenSpan flags every piece
 * of citro3d state the frame uses (target, viewport, effect block, program,
 * attribute and buffer layout, combiner, packet uniforms) so the span's first
 * draw re-emits all of it, and every draw binds its own texture.
 * Requires all three programs to keep stereo on f4 and stereoUi on f5 with no
 * other uniform or constant there (N3DS_StereoCopyCheckLayout, once at init).
 * A command the scan cannot walk, a 24-bit write to f4/f5, a target write that
 * is not the left target's, no target write at all, too little buffer, or the
 * atlas view falls back to the journal replay, which is always recorded.
 */
#define N3DS_STEREO_COPY_REG_STEREO   4u
#define N3DS_STEREO_COPY_REG_STEREOUI 5u
/* Words left free after the copy for the bottom-screen pass and FrameEnd's
 * own commands; a full buffer drops commands silently. */
#define N3DS_STEREO_COPY_SLACK_WORDS 16384u

static int s_stereoCopyLayoutOk = 0;
static int s_stereoCopyOpen = 0;
static u32 *s_stereoCopyBuf = NULL;
static u32 s_stereoCopyStart = 0;
static int s_stereoCopyLastOutcome = -2; /* logged on change only */

static int N3DS_StereoCopyCheckDvle(const DVLE_s *dvle, int needUi)
{
	u32 i;
	int stereoSeen = 0;
	int uiSeen = 0;

	if ((dvle == NULL) || (dvle->symbolTableData == NULL))
	{
		return 0;
	}
	for (i = 0; i < dvle->uniformTableSize; i++)
	{
		const DVLE_uniformEntry_s *u = &dvle->uniformTableData[i];
		const char *name = dvle->symbolTableData + u->symbolOffset;
		int lo;
		int hi;

		/* Float uniforms are registers 0x10-0x6F in the table. */
		if ((u->startReg < 0x10) || (u->startReg > 0x6F))
		{
			continue;
		}
		lo = (int)u->startReg - 0x10;
		hi = (int)u->endReg - 0x10;
		if (strcmp(name, "stereo") == 0)
		{
			if ((lo != (int)N3DS_STEREO_COPY_REG_STEREO) || (hi != lo))
			{
				return 0;
			}
			stereoSeen = 1;
		}
		else if (strcmp(name, "stereoUi") == 0)
		{
			if ((lo != (int)N3DS_STEREO_COPY_REG_STEREOUI) || (hi != lo))
			{
				return 0;
			}
			uiSeen = 1;
		}
		else if ((lo <= (int)N3DS_STEREO_COPY_REG_STEREOUI) && (hi >= (int)N3DS_STEREO_COPY_REG_STEREO))
		{
			return 0;
		}
	}
	for (i = 0; i < dvle->constTableSize; i++)
	{
		const DVLE_constEntry_s *c = &dvle->constTableData[i];

		if ((c->type == DVLE_CONST_FLOAT24) && ((c->id == N3DS_STEREO_COPY_REG_STEREO) || (c->id == N3DS_STEREO_COPY_REG_STEREOUI)))
		{
			return 0;
		}
	}
	return stereoSeen && (uiSeen || !needUi);
}

static int N3DS_StereoCopyCheckLayout(void)
{
	s_stereoCopyLayoutOk = 0;
	if ((s_shaderDvlb == NULL) || (s_trackShaderDvlb == NULL) || (s_modelShaderDvlb == NULL))
	{
		return 0;
	}
	if ((s_uLocStereo != (int)N3DS_STEREO_COPY_REG_STEREO) || (s_uLocStereoUi != (int)N3DS_STEREO_COPY_REG_STEREOUI) ||
	    (s_trackULocStereo != (int)N3DS_STEREO_COPY_REG_STEREO) || (s_modelULocStereo != (int)N3DS_STEREO_COPY_REG_STEREO))
	{
		return 0;
	}
	if (!N3DS_StereoCopyCheckDvle(&s_shaderDvlb->DVLE[0], 1) || !N3DS_StereoCopyCheckDvle(&s_trackShaderDvlb->DVLE[0], 0) ||
	    !N3DS_StereoCopyCheckDvle(&s_modelShaderDvlb->DVLE[0], 0))
	{
		return 0;
	}
	s_stereoCopyLayoutOk = 1;
	return 1;
}

/* End of N3DS_C3DBeginFrame, stereo frames only: the frame-start state is
 * already flagged (target and viewport by C3D_FrameDrawOn/C3D_SetViewport,
 * the effect block by C3D_DepthTest, the packet uniforms); this adds the
 * program, layout and combiner, then marks where the span begins. The program
 * goes through the model program first because citro3d does not re-emit a
 * rebind of the program already bound. */
static void N3DS_StereoCopyOpenSpan(void)
{
	u32 *buf = NULL;
	u32 size = 0;
	u32 offset = 0;

	s_stereoCopyOpen = 0;
	if (!s_stereoFrameActive || !s_stereoCopyLayoutOk || !g_dbg_stereoCopy)
	{
		return;
	}
	C3D_BindProgram(&s_modelShaderProgram);
	N3DS_BindPacketDrawState();
	GPUCMD_GetBuffer(&buf, &size, &offset);
	if (buf == NULL)
	{
		return;
	}
	s_stereoCopyBuf = buf;
	s_stereoCopyStart = offset;
	s_stereoCopyOpen = 1;
}

static void N3DS_StereoCopyRewriteVec(u32 *const words[4], u32 reg)
{
	/* C3D_FVec order in memory, and so in the data words: w, z, y, x. */
	float w;
	float z;
	float y;
	float x;

	memcpy(&w, words[0], sizeof(w));
	memcpy(&z, words[1], sizeof(z));
	memcpy(&y, words[2], sizeof(y));
	memcpy(&x, words[3], sizeof(x));
	if (reg == N3DS_STEREO_COPY_REG_STEREO)
	{
		const float nw = -z;
		const float nz = -w;

		w = nw;
		z = nz;
		y = -y;
	}
	x = -x;
	memcpy(words[0], &w, sizeof(w));
	memcpy(words[1], &z, sizeof(z));
	memcpy(words[2], &y, sizeof(y));
	memcpy(words[3], &x, sizeof(x));
}

/* Walks a span of complete PICA commands (param, header, extra params, pad
 * to 8 bytes) and rewrites the right eye's words in place. Returns 0 if the
 * span cannot be walked or holds a write this rewrite does not handle. */
static int N3DS_StereoCopyRewrite(u32 *w, u32 n, u32 *outTargetWords, u32 *outUniformVecs)
{
	const u32 leftDepth = osConvertVirtToPhys(s_renderTarget->frameBuf.depthBuf) >> 3;
	const u32 leftColor = osConvertVirtToPhys(s_renderTarget->frameBuf.colorBuf) >> 3;
	const u32 rightDepth = osConvertVirtToPhys(s_renderTargetRight->frameBuf.depthBuf) >> 3;
	const u32 rightColor = osConvertVirtToPhys(s_renderTargetRight->frameBuf.colorBuf) >> 3;
	u32 *vec[4] = {NULL, NULL, NULL, NULL};
	u32 uniReg = 0;
	u32 uniWord = 0;
	int uniF32 = 0;
	int uniValid = 0;
	u32 depthSeen = 0;
	u32 colorSeen = 0;
	u32 vecs = 0;
	u32 i = 0;

	while (i < n)
	{
		u32 header;
		u32 reg;
		u32 mask;
		u32 extra;
		u32 consec;
		u32 p;

		if (i + 1u >= n)
		{
			return 0;
		}
		header = w[i + 1u];
		reg = header & 0xFFFFu;
		mask = (header >> 16) & 0xFu;
		extra = (header >> 20) & 0x7FFu;
		consec = header >> 31;
		if (i + 2u + extra > n)
		{
			return 0;
		}
		for (p = 0; p <= extra; p++)
		{
			u32 *word = (p == 0) ? &w[i] : &w[i + 1u + p];
			const u32 r = consec ? (reg + p) : reg;

			if (r == GPUREG_VSH_FLOATUNIFORM_CONFIG)
			{
				if (mask != 0xFu)
				{
					return 0;
				}
				uniReg = *word & 0x7Fu;
				uniF32 = (int)(*word >> 31);
				uniWord = 0;
				uniValid = 1;
			}
			else if ((r >= GPUREG_VSH_FLOATUNIFORM_DATA) && (r <= GPUREG_VSH_FLOATUNIFORM_DATA + 7u))
			{
				const u32 per = uniF32 ? 4u : 3u;
				const int eyeReg = (uniReg == N3DS_STEREO_COPY_REG_STEREO) || (uniReg == N3DS_STEREO_COPY_REG_STEREOUI);

				if (!uniValid || (mask != 0xFu) || (eyeReg && !uniF32))
				{
					return 0;
				}
				if (eyeReg)
				{
					vec[uniWord] = word;
				}
				uniWord++;
				if (uniWord == per)
				{
					if (eyeReg)
					{
						N3DS_StereoCopyRewriteVec(vec, uniReg);
						vecs++;
					}
					uniReg++;
					uniWord = 0;
				}
			}
			else if (r == GPUREG_DEPTHBUFFER_LOC)
			{
				if ((mask != 0xFu) || (*word != leftDepth))
				{
					return 0;
				}
				*word = rightDepth;
				depthSeen++;
			}
			else if (r == GPUREG_COLORBUFFER_LOC)
			{
				if ((mask != 0xFu) || (*word != leftColor))
				{
					return 0;
				}
				*word = rightColor;
				colorSeen++;
			}
		}
		i += 2u + extra + (extra & 1u);
	}
	if ((i != n) || (depthSeen == 0) || (colorSeen == 0))
	{
		return 0;
	}
	*outTargetWords = depthSeen + colorSeen;
	*outUniformVecs = vecs;
	return 1;
}

#ifdef CTR_INTERNAL
extern int g_dbg_atlasView;
#endif

/* Returns 1 when the right eye was drawn by the copy; 0 means replay it. */
static int N3DS_StereoCopyRightEye(void)
{
	u32 *buf = NULL;
	u32 size = 0;
	u32 end = 0;
	u32 n = 0;
	u32 targetWords = 0;
	u32 uniformVecs = 0;
	int outcome = 0; /* 0 = copied; >0 = why it replayed */

	if (!s_stereoCopyOpen)
	{
		outcome = (s_stereoCopyLayoutOk && g_dbg_stereoCopy) ? 4 : (s_stereoCopyLayoutOk ? 2 : 1);
	}
#ifdef CTR_INTERNAL
	else if (g_dbg_atlasView > 0)
	{
		/* The atlas view is a left-eye-only takeover the journal omits. */
		outcome = 3;
	}
#endif
	if (outcome == 0)
	{
		GPUCMD_GetBuffer(&buf, &size, &end);
		if ((buf != s_stereoCopyBuf) || (end <= s_stereoCopyStart))
		{
			outcome = 4;
		}
		else
		{
			n = end - s_stereoCopyStart;
			if ((u64)end + (u64)n + N3DS_STEREO_COPY_SLACK_WORDS > (u64)size)
			{
				outcome = 5;
			}
		}
	}
	if (outcome == 0)
	{
		memcpy(buf + end, buf + s_stereoCopyStart, (size_t)n * sizeof(u32));
		if (N3DS_StereoCopyRewrite(buf + end, n, &targetWords, &uniformVecs))
		{
			GPUCMD_SetBufferOffset(end + n);
		}
		else
		{
			/* The offset never moved, so the partial copy is not part of the
			 * command list. */
			outcome = 6;
		}
	}
	s_stereoCopyOpen = 0;

	if (outcome != s_stereoCopyLastOutcome)
	{
		static const char *const why[7] = {"copy",
		                                   "replay (shader layout check failed)",
		                                   "replay (stereocopy 0)",
		                                   "replay (atlas view)",
		                                   "replay (command span missing or moved)",
		                                   "replay (command buffer too full)",
		                                   "replay (span not rewritable)"};
		char msg[128];

		s_stereoCopyLastOutcome = outcome;
		if (outcome == 0)
		{
			snprintf(msg, sizeof(msg), "[CTR-3DS] stereo right eye: copy (span %u words, %u target words, %u parallax vectors rewritten)", (unsigned)n,
			         (unsigned)targetWords, (unsigned)uniformVecs);
		}
		else
		{
			snprintf(msg, sizeof(msg), "[CTR-3DS] stereo right eye: %s", why[outcome]);
		}
		N3DS_DebugLog(msg);
	}
	if (outcome != 0)
	{
		return 0;
	}

	/* The GPU now holds the left eye's end state with the right target and
	 * the right eye's parallax. Match citro3d's view of the target (and mark
	 * it used, so FrameEnd transfers it), and leave later uploads on the right
	 * eye, as the replay does. */
	s_stereoRecording = 0;
	s_stereoEye = 1;
	C3D_FrameDrawOn(s_renderTargetRight);
	C3D_SetViewport(0, g_dbg_widescreen ? 0 : N3DS_TOP_CONTENT_X, 240, g_dbg_widescreen ? 400 : N3DS_TOP_CONTENT_W);
	return 1;
}

void N3DS_StereoRenderRightEye(void)
{
	int i;
	int lastDoubling = -1;
	int lastDepthWrite = 1;

	if (!s_stereoFrameActive || (s_renderTargetRight == NULL))
	{
		return;
	}

	if (N3DS_StereoCopyRightEye())
	{
		return;
	}

	s_stereoRecording = 0;
	s_stereoEye = 1;

	C3D_FrameDrawOn(s_renderTargetRight);
	/* Same pillarbox crop (or widescreen lift) as N3DS_C3DBeginFrame --
	 * viewport state does not automatically follow the framebuffer binding. */
	C3D_SetViewport(0, g_dbg_widescreen ? 0 : N3DS_TOP_CONTENT_X, 240, g_dbg_widescreen ? 400 : N3DS_TOP_CONTENT_W);
	/* Frame-wide depth pair, mirroring BeginFrame's policy exactly --
	 * whatever state the left eye's last draw left behind is not it. */
	if (g_dbg_depthTest == 1)
	{
		C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
	}
	else
	{
		C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
	}
	/* Packet program + layout + buffer + combiner + projection + the RIGHT
	 * eye's packet parallax (s_stereoEye is 1 now, so the upload inside
	 * picks the right vector). */
	N3DS_BindPacketDrawState();

	if (s_stereoOverflow)
	{
		static int s_overflowLogged = 0;

		if (!s_overflowLogged)
		{
			s_overflowLogged = 1;
			N3DS_DebugLog("[CTR-3DS] stereo: journal OVERFLOW, right eye truncated (raise N3DS_STEREO_MAX_OPS)");
		}
	}

	for (i = 0; i < s_stereoOpCount; i++)
	{
		const N3DS_StereoOp *op = &s_stereoOps[i];

		switch (op->kind)
		{
		case N3DS_STEREO_OP_PKT:
		{
			if ((int)op->u.pkt.doubling != lastDoubling)
			{
				N3DS_C3DSetColorDoubling((int)op->u.pkt.doubling);
				lastDoubling = (int)op->u.pkt.doubling;
			}
			if ((int)op->u.pkt.depthWrite != lastDepthWrite)
			{
				N3DS_C3DSetDepthWrite((int)op->u.pkt.depthWrite);
				lastDepthWrite = (int)op->u.pkt.depthWrite;
			}
			N3DS_C3DSetBlendMode((int)op->u.pkt.blend);
			N3DS_C3DBindAtlas((int)op->u.pkt.atlas);
			C3D_DrawArrays(GPU_TRIANGLES, op->u.pkt.baseVertex, op->u.pkt.vertCount);
			break;
		}
		case N3DS_STEREO_OP_VRAM:
		{
			/* Mirrors PresentVRAMRect's own draw: doubling off, dedicated
			 * texture, quad at N3DS_DEPTH_TOPMOST (zero parallax via the
			 * shader's UI-band mask, so both eyes see it at screen depth).
			 * The slot's texels persist to frame end -- see the header. */
			if (lastDoubling != 0)
			{
				N3DS_C3DSetColorDoubling(0);
				lastDoubling = 0;
			}
			N3DS_C3DBindVramDisplayTex((int)op->u.pkt.atlas);
			C3D_DrawArrays(GPU_TRIANGLES, op->u.pkt.baseVertex, op->u.pkt.vertCount);
			break;
		}
		case N3DS_STEREO_OP_TRACK_BEGIN:
		{
			N3DS_TrackDraw_Begin(op->u.mtx.qx, op->u.mtx.qy, op->u.mtx.qw, op->u.mtx.h);
			lastDoubling = -1; /* the bracket owns doubling/blend/depth-write */
			break;
		}
		case N3DS_STEREO_OP_TRACK_LEAF:
		{
			N3DS_TrackDraw_Leaf(op->u.leaf.firstIndex, op->u.leaf.indexCount, op->u.leaf.otIndexLo, op->u.leaf.ordinalLo, op->u.leaf.ordinalSpan,
			                    op->u.leaf.invIndexSpan, op->u.leaf.atlasBin, op->u.leaf.blend, op->u.leaf.uvDx, op->u.leaf.uvDy);
			break;
		}
		case N3DS_STEREO_OP_TRACK_END:
		{
			N3DS_TrackDraw_End();
			lastDoubling = -1;
			lastDepthWrite = 1; /* End restores the frame-wide WRITE_ALL pair */
			break;
		}
		case N3DS_STEREO_OP_MODEL_BEGIN:
		{
			N3DS_ModelDraw_Begin();
			lastDoubling = -1;
			break;
		}
		case N3DS_STEREO_OP_MODEL_INSTANCE:
		{
			N3DS_ModelDraw_InstanceImpl(op->u.mtx.qx, op->u.mtx.qy, op->u.mtx.qw, op->u.mtx.h, op->u.mtx.skyDome);
			break;
		}
		case N3DS_STEREO_OP_MODEL_RUN:
		{
			N3DS_ModelDraw_Run(op->u.mrun.startVertex, op->u.mrun.triCount, op->u.mrun.atlas, op->u.mrun.textured, op->u.mrun.blend);
			break;
		}
		case N3DS_STEREO_OP_MODEL_END:
		{
			N3DS_ModelDraw_End();
			lastDoubling = -1;
			lastDepthWrite = 1;
			break;
		}
		default:
		{
			break;
		}
		}
	}

	/* Defensive: a truncated journal can end inside a bracket, leaving the
	 * track/model program bound. Cheap, idempotent, and the next BeginFrame
	 * re-asserts the rest of the frame-wide state regardless. */
	if (s_stereoOverflow)
	{
		N3DS_BindPacketDrawState();
		if (g_dbg_depthTest != 1)
		{
			C3D_DepthTest(true, GPU_LEQUAL, GPU_WRITE_ALL);
		}
	}

	/* s_stereoEye stays 1 until the next frame's latch resets it -- nothing
	 * else draws between here and N3DS_C3DEndFrame. */
}

/* Depth-WRITE only; the test itself stays LEQUAL so translucent content is
 * still correctly hidden by anything in front of it. Callers memoize (see
 * N3DS_MergeEmit), so this is not gated again here. Under a build without
 * OT_DEPTH there is no depth buffer to mask and this is a no-op. */
void N3DS_C3DSetDepthWrite(int enable)
{
	C3D_DepthTest(true, GPU_LEQUAL, enable ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
}

void N3DS_C3DSetBlendMode(int blendMode)
{
	static int lastMode = -1;

	if (blendMode == lastMode)
	{
		return;
	}
	lastMode = blendMode;

	switch (blendMode)
	{
	default:
	case 0:
	{ /* BM_NONE: opaque */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
		break;
	}
	case 1:
	{ /* BM_AVERAGE: 0.5*dst + 0.5*src, via constant alpha = 0.5 */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA, GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
		break;
	}
	case 2:
	{ /* BM_ADD: dst + src */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
		break;
	}
	case 3:
	{ /* BM_SUBTRACT: dst - src (reverse-subtract, same as PC's GL_FUNC_REVERSE_SUBTRACT) */
		C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
		break;
	}
	case 4:
	{ /* BM_ADD_QUATER_SOURCE: dst + 0.25*src, via constant color = 0.25 */
		C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_COLOR, GPU_ONE, GPU_ONE, GPU_ZERO);
		break;
	}
	}
}

void N3DS_C3DUploadVramDisplayChunk(int slotIndex, const void *tiled256)
{
	if ((slotIndex < 0) || (slotIndex >= N3DS_VRAM_DISPLAY_TEX_COUNT))
	{
		return;
	}

	C3D_TexUpload(&s_vramDisplayTex[slotIndex], tiled256);
}

void N3DS_C3DBindVramDisplayTex(int slotIndex)
{
	if ((slotIndex < 0) || (slotIndex >= N3DS_VRAM_DISPLAY_TEX_COUNT))
	{
		return;
	}

	C3D_TexBind(0, &s_vramDisplayTex[slotIndex]);
}

#ifdef CTR_INTERNAL
extern int g_dbg_texturelessMode;
/* Defined in the unity TU (native_renderer_3ds.c) -- the cache arrays live
 * there and this TU can't see them (deliberate TU split, see the file
 * header). void(void), so no ABI hazard across the enum-width boundary. */
void NativeRenderer3DS_DebugDumpCacheMeta(void);

void NativeRenderer3DS_DebugArmTriangleDump(void);

/* Runs the full debug capture set (dumpreq `capture`): emulated VRAM, every atlas's raw
 * storage, and both decode-cache tiers' metadata, all describing the same
 * instant, plus arms a per-triangle dump of the NEXT frame (page/clut/UV/
 * position/atlas-resolution per triangle -- the addressing-side complement
 * to the content-side dumps; content was proven byte-correct while
 * the screen quilts, so the divergence must be visible here). One entry
 * point so the button gesture and the host-side file trigger below can
 * never drift apart in what they capture. */
static void N3DS_DebugCaptureAll(void)
{
	N3DS_DebugWriteVRAMDump();
	N3DS_DebugWriteAtlasDump();
	NativeRenderer3DS_DebugDumpCacheMeta();
	NativeRenderer3DS_DebugArmTriangleDump();
}
#endif

unsigned int N3DS_ReadButtons(void)
{
	unsigned int out = 0;
	u32 held;

	hidScanInput();
	held = hidKeysHeld();

#ifdef CTR_INTERNAL
	/* SELECT+R freed (the VRAM/debug capture set). Still reachable as dumpreq `capture`. */

	/* Host-scriptable trigger: Azahar's SD card is a plain host
	 * folder, so an autonomous session can drive diagnostics by creating
	 * sdmc:/dumpreq.txt -- no window focus or key injection involved. The
	 * file's first word selects the action (the A/B toggles need
	 * to be reachable without a gamepad):
	 *   capture (or anything else) -- the full SELECT+R capture set
	 *   subpage     -- toggle g_dbg_disableSubPage (route all to full-page tier)
	 *   single      -- toggle g_dbg_forceSingleTexture
	 *   textureless -- toggle g_dbg_texturelessMode
	 *   triscopes   -- toggle g_dbg_perfTriScopes (per-triangle perf split; taxed)
	 *   flagstyle [N] -- menu/loading flag: 0 = retail grey checkers (default),
	 *                  1 = pride stripes, 2 = purple/white (Spyro). Bare token
	 *                  cycles. Also reachable in-game by cheat: WONDER, SOAR.
	 *   pentapal [N] -- opt-in: give Penta his PAL/Japan max-stats (5/5/5) class
	 *                  instead of the NTSC-U Turn class. Bare token toggles, N sets
	 *                  0/1. Off = retail NTSC-U. Applied at kart birth.
	 *   reserves [N] -- boost-reserves HUD meter by the retail slide bar
	 *                  (DEFAULT ON). Bare token toggles, N sets 0/1.
	 *   tirefix [N] -- fix retail's mirrored-tire delay-slot bug so ice
	 *                  reflections show wheels at the corners (DEFAULT ON).
	 *                  OFF = the shipped PS1 centerline tires. Bare token
	 *                  toggles, N sets 0/1.
	 *   renderthread -- toggle g_dbg_renderThread (core-2 render worker routing;
	 *                  OFF falls back to synchronous main-thread rendering)
	 *   lodgeo [N]  -- track LOD distance, percent of retail (CTR_NATIVE_LOD_BIAS)
	 *   lodtex [N]  -- texture LOD distance, percent of retail (same gate)
	 *   fbtest      -- toggle g_dbg_skipFbTriTest (skip resolve's
	 *                  per-triangle fb-mirror test; freeze frames stale while ON)
	 *   otz         -- toggle g_dbg_otzWiden (widened depth stamps, track-
	 *                  renderer rung 1; [OTZ] occupancy line while on)
	 *   trackrender [N] -- native track renderer state: 0=off(retail)
	 *                  1=on(resident replaces retail)
	 *                  2=both(diagnostic overlay). Bare token cycles 0->1->2->0;
	 *                  'trackrender N' sets a specific state. The SELECT+START
	 *                  master flips it with the rest of the renderer config.
	 *   stereocopy [N] -- g_dbg_stereoCopy: 1 (default) draws the right eye
	 *                  from a rewritten copy of the left eye's command list;
	 *                  0 replays the journal. Bare token toggles. Takes effect
	 *                  next frame.
	 *   skygpu [N]  -- g_dbg_skyGpu: 1 (default) draws the sky dome on the
	 *                  GPU from its level mesh; 0 is retail's CPU-projected
	 *                  packets. Bare token toggles. Takes effect next frame.
	 *   packetdepth -- toggle g_dbg_packetDepth (depth model): packets stamp
	 *                  GTE sz3 and the track shader switches to the same
	 *                  true-depth axis, both at once. Button: folded into the
	 *                  SELECT+START master.
	 *   gtecapture [N] -- g_dbg_gteCapture:
	 *                  1=full 2=store-only(no matrix note) 0=ablated, to price
	 *                  the capture subsystem and its matrix probe separately.
	 *                  Bare token cycles 1->2->0.
	 *   stereo      -- toggle g_dbg_stereo (master 3D enable; the physical slider
 *                  still gates frame by frame, this just forces mono)
 *   stereowindow -- toggle window mode (default OFF per owner: pop-out is
 *                  cooler; ON = no pop-out, world starts at the screen)
 *   stereosep [N] -- full-slider disparity at infinite depth, PSX px
 *                  (default 12; negative swaps the eyes; bare resets)
 *   stereoconv [N] -- screen-plane depth, view.z units (default 1200;
 *                  bare resets)
 *   stereopop [N] -- pop-out clamp as a multiple of the at-infinity shift
 *                  (default 2; 0 pins near content to the screen, 65536 =
 *                  uncapped; bare resets). Button: SELECT+D-LEFT toggles
 *                  capped <-> uncapped.
 *   depthtest [N] -- g_dbg_depthTest, the occlusion split: 0=normal
	 *                  1=no depth test (painter's order) 2=track tests but
	 *                  does not write depth. Bare token cycles 0->1->2->0.
	 *   wide [N]    -- g_dbg_widescreen: bare toggles off<->REAL (mode 2,
	 *                  wide FOV window at 4:3 pixel scale); `wide 1` is the
	 *                  rejected anamorphic stretch. Button: folded into the
	 *                  SELECT+START master.
	 *   hudshine [N] -- g_dbg_hudShine: the juiced-wumpa HUD shine takes the
	 *                  UI band (widescreen ramp, stereo flatten, seam inset)
	 *                  although retail links it into the player's OT. Bare
	 *                  toggles, N sets 0/1. Default ON.
	 *   screenfade [N] -- g_dbg_screenFade: retail's per-player full-window
	 *                  fade (hit flash, finish-line flash) draws in the
	 *                  ordered UI pass across the whole wide window instead
	 *                  of depth-testing against the world. Bare toggles, N
	 *                  sets 0/1. Default ON.
	 *   bilinear    -- toggle g_dbg_trackBilinear (default OFF): track-atlas
	 *                  sampler bilinear <-> nearest. Live, no rebuild.
	 *                  Button: SELECT+D-RIGHT.
	 *   alpharef N  -- punch-through cutoff 0-255 (g_dbg_trackAlphaRef, default
	 *                  1). Raising it trims bilinear's dark cutout rim; too high
	 *                  erodes thin cutout detail. No-op under nearest. Bare = 1.
	 *   padmux [N]  -- virtual multitap (default OFF -- renderer can't do
	 *                  split-screen, and off keeps VS/battle behind retail's
	 *                  own controller gate): `padmux N` drives pad N with the
	 *                  3DS buttons, 0 = off (retail single-pad bus), bare =
	 *                  cycle the driven pad.
	 *   vol [N]     -- output-stage gain, percent of SPU-faithful amplitude
	 *                  (retail's 0x3fff master is half digital scale, so 200
	 *                  = full scale and the default). Bare resets to default.
	 *   audiothread -- stop and join the audio worker thread at runtime
	 *                  (one-way, no restart). The ndsp sink stays up;
	 *                  NativeAudio_StepVBlank's main-thread fallback keeps
	 *                  audio audible.
	 *   ndspoff     -- stop the audio worker (if running) and tear down the
	 *                  ndsp sink permanently (N3DS_NdspForceOff) -- audio stays
	 *                  silent for the rest of the run, every
	 *                  ndspChn.../ndspWaveBuf/ndspSetOutputMode call site is
	 *                  gated on the sink-up flag already, and a later XA play
	 *                  request cannot bring it back.
	 *   nextboot    -- boot barrier: stop consuming lines until the next power-on.
	 *   speedoff    -- osSetSpeedupEnable(false) -- drops the New3DS 804 MHz
	 *                  clock-up back to O3DS speed.
	 * Polled every 64 frames so the SD probe cost stays negligible on real
	 * hardware; the request file is deleted first so one request fires once. */
	{
		/* 1, not 64: the first poll happens on the first button read of the
		 * boot so a request staged on the card before launch applies ahead of
		 * the boot-time loads (speedoff), not a second into the intro. */
		static unsigned int s_dumpPollCountdown = 1;
		extern int NativeMemcard_WritesInFlight(void);

		/* A save write holds the SD card for seconds, and this probe would wait behind it on the
		 * game thread: a due probe stays due until the save writer is idle. */
		if ((s_dumpPollCountdown > 1) || !NativeMemcard_WritesInFlight())
		{
			s_dumpPollCountdown--;
		}

		if (s_dumpPollCountdown == 0)
		{
			FILE *req = fopen("sdmc:/dumpreq.txt", "rb");

			s_dumpPollCountdown = 64;
			if (req != NULL)
			{
				extern int g_dbg_disableSubPage;
				extern int g_dbg_forceSingleTexture;
				/* The file holds one request per line; each poll consumes the
				 * FIRST line only and writes the remainder back, so a multi-line
				 * file drains one request per poll (~64 frames apart) in file
				 * order. A word is at most 23 chars ("stereoconv 12000" is 16). */
				char word[24];
				char file[256];
				size_t got = fread(file, 1, sizeof(file) - 1, req);
				size_t lineLen = 0;
				const char *rest;
				int consumed = 0;

				file[got] = '\0';
				fclose(req);

				while ((file[lineLen] != '\0') && (file[lineLen] != '\n') && (file[lineLen] != '\r'))
				{
					lineLen++;
				}
				if (lineLen > sizeof(word) - 1)
				{
					lineLen = sizeof(word) - 1;
				}
				memcpy(word, file, lineLen);
				word[lineLen] = '\0';

				rest = file + lineLen;
				while ((*rest == '\n') || (*rest == '\r') || (*rest == ' '))
				{
					rest++;
				}

				/* Consumed = the first line is gone from the card. On a host
				 * filesystem (Azahar) a freshly written file can be transiently
				 * held open by the OS (AV scan); the remove()/rewrite then fails,
				 * and acting anyway re-fires the same request every poll -- a
				 * TOGGLE request flapped a dozen in-place rebuilds that way. Skip
				 * this poll instead: the file survives and the next poll retries. */
				if (*rest == '\0')
				{
					consumed = (remove("sdmc:/dumpreq.txt") == 0);
				}
				else
				{
					FILE *out = fopen("sdmc:/dumpreq.txt", "wb");

					if (out != NULL)
					{
						consumed = (fputs(rest, out) >= 0);
						fclose(out);
					}
				}

				if (!consumed)
				{
					/* not consumed -- retried next poll */
				}
				else if (strncmp(word, "widemask", 8) == 0)
				{
					/* Strips-hunt kill switches: bit 1 = instance bbox
					 * margin, 2 = per-tri margin, 4 = sky face margin,
					 * 8 = sky extra sectors, 16 = BSP frustum corner X.
					 * `widemask N` (decimal 0-31) sets; bare resets to
					 * all-on. MUST be matched before the `wide` token --
					 * shared prefix. */
					extern int g_dbg_wideMarginMask;
					char msg[48];

					g_dbg_wideMarginMask = (word[8] == ' ') ? (atoi(&word[9]) & 0x1F) : 0x1F;
					snprintf(msg, sizeof(msg), "[CTR-3DS] wide margin mask: 0x%X", (unsigned)g_dbg_wideMarginMask);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "wide", 4) == 0)
				{
					/* Bare token toggles off<->REAL. `wide N` (0/1/2) sets a
					 * mode directly -- 1 is the rejected anamorphic stretch,
					 * kept reachable only this explicit way. */
					if (word[4] == ' ' && word[5] >= '0' && word[5] <= '2')
					{
						g_dbg_widescreen = word[5] - '0';
					}
					else
					{
						g_dbg_widescreen = (g_dbg_widescreen != 0) ? 0 : 2;
					}
					N3DS_DebugLog((g_dbg_widescreen == 2)   ? "[CTR-3DS] widescreen: REAL (wide FOV window, 4:3 pixel scale)"
					              : (g_dbg_widescreen == 1) ? "[CTR-3DS] widescreen: STRETCH (anamorphic)"
					                                        : "[CTR-3DS] widescreen: off (4:3 crop)");
				}
				else if (strncmp(word, "audiothread", 11) == 0)
				{
					/* Rosalina-freeze bisect: stop and join the core-1 audio
					 * worker at runtime, leaving the ndsp sink up so
					 * NativeAudio_StepVBlank's main-thread fallback keeps
					 * audio audible. One-way -- the worker does not restart. */
					extern void N3DS_AudioThreadStopAndJoin(void);

					N3DS_AudioThreadStopAndJoin();
					N3DS_DebugLog("[CTR-3DS] audiothread: STOPPED (main-thread fallback)");
				}
				else if (strncmp(word, "ndspoff", 7) == 0)
				{
					/* Rosalina-freeze bisect round 2: permanently release the
					 * DSP -- N3DS_NdspForceOff stops the audio worker (if
					 * running), tears the sink down, and latches it so a
					 * later XA play request cannot reinitialize ndsp. */
					extern void N3DS_NdspForceOff(void);

					N3DS_NdspForceOff();
					N3DS_DebugLog("[CTR-3DS] ndspoff: DSP released");
				}
				else if (strncmp(word, "nextboot", 8) == 0)
				{
					/* Boot barrier: the lines after this one stay on the card
					 * for the NEXT power-on, so one file can script several
					 * boots without a trip to the SD card in between. */
					s_dumpPollCountdown = 0x7fffffffu;
					N3DS_DebugLog("[CTR-3DS] nextboot: dumpreq polling stopped for this boot");
				}
				else if (strncmp(word, "speedoff", 8) == 0)
				{
					/* Rosalina-freeze bisect round 2: drop the New3DS
					 * 804MHz clock-up back to O3DS speed. */
					osSetSpeedupEnable(false);
					N3DS_DebugLog("[CTR-3DS] speedoff: New3DS clock-up disabled");
				}
				else if (strncmp(word, "sdlog", 5) == 0)
				{
					g_dbg_sdLog ^= 1;
					N3DS_DebugLog(g_dbg_sdLog ? "[CTR-3DS] sdlog: ON (writing ctr3ds.log -- costs a ~2s hitch)"
					                          : "[CTR-3DS] sdlog: off (no SD writes; bottom screen still live)");
				}
				else if (strncmp(word, "subpage", 7) == 0)
				{
					void NativeRenderer3DS_DebugInvalidateCaches(void);

					g_dbg_disableSubPage ^= 1;
					/* Both tiers share physical cells while the diagnostic
					 * bound is widened -- drop all cached decodes so neither
					 * regime serves the other's overwrites. */
					NativeRenderer3DS_DebugInvalidateCaches();
					N3DS_DebugLog(g_dbg_disableSubPage ? "[CTR-3DS] sub-page tier: DISABLED (full-page only)" : "[CTR-3DS] sub-page tier: enabled");
				}
				else if (strncmp(word, "single", 6) == 0)
				{
					g_dbg_forceSingleTexture ^= 1;
					N3DS_DebugLog(g_dbg_forceSingleTexture ? "[CTR-3DS] force single texture: ON" : "[CTR-3DS] force single texture: off");
				}
				else if (strncmp(word, "textureless", 11) == 0)
				{
					g_dbg_texturelessMode ^= 1;
					N3DS_DebugLog(g_dbg_texturelessMode ? "[CTR-3DS] textureless mode: 1" : "[CTR-3DS] textureless mode: 0");
				}
				else if (strncmp(word, "triscopes", 9) == 0)
				{
					extern int g_dbg_perfTriScopes;

					g_dbg_perfTriScopes ^= 1;
					N3DS_DebugLog(g_dbg_perfTriScopes ? "[CTR-3DS] per-tri perf scopes: ON (syscall tax ~10k/frame)"
					                                  : "[CTR-3DS] per-tri perf scopes: off");
				}
#if defined(CTR_NATIVE_LOD_BIAS) && CTR_NATIVE_LOD_BIAS
				else if (strncmp(word, "lodgeo", 6) == 0 || strncmp(word, "lodtex", 6) == 0)
				{
					/* Track detail ('lodgeo') and texture detail ('lodtex') as a
					 * percentage of retail switch distance -- lower means detail
					 * drops nearer the camera and the OT walk is handed fewer
					 * prims. 'lodgeo 50' sets a value; bare 'lodgeo' steps the
					 * A/B ladder. Read once per frame when RenderAllLevelGeometry
					 * seeds its thresholds, so a change lands on a frame boundary
					 * and never mid-walk. */
					extern int g_ctrLodGeoScale;
					extern int g_ctrLodTexScale;
					extern int CTR_LodBias_Clamp(int percent);
					extern int CTR_LodBias_Step(int percent);
					int isGeo = (word[3] == 'g');
					int *scale = isGeo ? &g_ctrLodGeoScale : &g_ctrLodTexScale;
					char msg[48];

					*scale = (word[6] == ' ') ? CTR_LodBias_Clamp(atoi(&word[7])) : CTR_LodBias_Step(*scale);
					snprintf(msg, sizeof(msg), "[CTR-3DS] %s LOD scale: %d%%", isGeo ? "level" : "texture", *scale);
					N3DS_DebugLog(msg);
				}
#endif
				else if (strncmp(word, "renderthread", 12) == 0)
				{
					extern int g_dbg_renderThread;

					/* Checked at post time on the game thread; the OFF
					 * transition fences the worker before the first direct
					 * call (NativeRenderQueue_Route), so the flip is safe
					 * mid-race. This is the feature's A/B instrument. */
					g_dbg_renderThread ^= 1;
					N3DS_DebugLog(g_dbg_renderThread ? "[CTR-3DS] render worker routing: ON"
					                                 : "[CTR-3DS] render worker routing: OFF (sync on main thread)");
				}
				else if (strncmp(word, "bilinear", 8) == 0)
				{
					/* Same live filter flip as the SELECT+D-RIGHT chord. */
					g_dbg_trackBilinear ^= 1;
					N3DS_TrackAtlas_ApplyFilter();
					N3DS_DebugLog(g_dbg_trackBilinear ? "[CTR-3DS] track filter: BILINEAR" : "[CTR-3DS] track filter: nearest");
				}
				else if (strncmp(word, "alpharef", 8) == 0)
				{
					/* Punch-through cutoff (0-255), the bilinear-rim dial. Set
					 * once at init, so re-assert C3D_AlphaTest here to make the
					 * new reference land live. Bare token resets to 1. */
					const char *arg = word + 8;
					int ref = 1;

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						ref = atoi(arg);
						if (ref < 0)
						{
							ref = 0;
						}
						if (ref > 255)
						{
							ref = 255;
						}
					}
					g_dbg_trackAlphaRef = ref;
					C3D_AlphaTest(true, GPU_GEQUAL, g_dbg_trackAlphaRef);
					{
						char msg[64];
						snprintf(msg, sizeof(msg), "[CTR-3DS] alpha cutoff: %d/255 (1 = retail; higher trims bilinear rim)", g_dbg_trackAlphaRef);
						N3DS_DebugLog(msg);
					}
				}
				else if (strncmp(word, "padmux", 6) == 0)
				{
					/* Virtual multitap (native_input.c): all four pads report
					 * connected so VS/battle player counts open up; the 3DS
					 * buttons drive one selectable pad. `padmux 0` = off
					 * (retail single-pad bus), `padmux N` = drive pad N, bare
					 * = cycle the driven pad. */
					extern int Platform_InputDebugPadMux(int op);
					const char *arg = word + 6;
					int op = -1;
					int state;
					char msg[80];

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						op = atoi(arg);
					}
					state = Platform_InputDebugPadMux(op);
					if (state == 0)
					{
						N3DS_DebugLog("[CTR-3DS] padmux: OFF (retail single-pad bus)");
					}
					else
					{
						snprintf(msg, sizeof(msg), "[CTR-3DS] padmux: 4 pads connected, driving pad %d", state);
						N3DS_DebugLog(msg);
					}
				}
				else if (strncmp(word, "vol", 3) == 0)
				{
					/* Output-stage gain (n3ds_audio_thread.c): applied at the
					 * ndsp mix pokes only, so the mixer -- and PC byte-identity
					 * -- is untouched. 'vol N' sets percent of SPU-faithful
					 * amplitude; bare resets to the 200 default. */
					extern int N3DS_AudioSetOutputGainPercent(int percent);
					const char *arg = word + 3;
					int pct = -1;
					char msg[64];

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						pct = atoi(arg);
					}
					pct = N3DS_AudioSetOutputGainPercent(pct);
					snprintf(msg, sizeof(msg), "[CTR-3DS] output gain: %d%% (100 = SPU-faithful)", pct);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "stats", 5) == 0)
				{
					extern int g_dbg_statsConsole;

					/* Bottom-console visibility (release default OFF): the
					 * stats drip and Platform_Log's stdout echo both paint
					 * again while ON. Log/SD/netload sinks are unaffected
					 * either way. */
					g_dbg_statsConsole ^= 1;
					N3DS_DebugLog(g_dbg_statsConsole ? "[CTR-3DS] stats console: ON (bottom panel shows stats + log echo)"
					                                 : "[CTR-3DS] stats console: off (bottom panel quiet outside the map)");
				}
				else if (strncmp(word, "btmmap", 6) == 0)
				{
					extern int g_dbg_btmMap;

					/* Bottom-screen minimap. ON restamps the in-race map block
					 * one below the UI pin and replays it on the bottom render
					 * target, magnified; the debug console yields the panel
					 * while the map is live. OFF leaves every stamp at the pin
					 * and the map draws on the top screen exactly as before --
					 * and the console (stats block) gets the panel back. */
					g_dbg_btmMap ^= 1;
					N3DS_DebugLog(g_dbg_btmMap ? "[CTR-3DS] btmmap: ON (minimap on the bottom screen)"
					                           : "[CTR-3DS] btmmap: off (minimap on the top screen, console restored)");
				}
				else if (strncmp(word, "mapboth", 7) == 0)
				{
					extern int g_dbg_mapBoth;
					const char *arg = word + 7;

					/* Map-mode top copy: with the speedo toggled off (or in
					 * 3P), the map draws on BOTH screens -- retail position on
					 * top, magnified on the bottom. OFF = move-only routing
					 * (bottom screen only), today's behaviour. Speedo mode is
					 * never affected: its map block only exists because of
					 * the route override and stays off the top screen.
					 * 'mapboth 0/1' sets, bare 'mapboth' toggles. */
					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						/* 2 = diagnostic: copy WITHOUT the retailTop mode
						 * signal (also copies in speedo mode). */
						g_dbg_mapBoth = atoi(arg);
					}
					else
					{
						g_dbg_mapBoth = !g_dbg_mapBoth;
					}
					{
						char msg[96];

						snprintf(msg, sizeof(msg), "[CTR-3DS] mapboth: %d %s", g_dbg_mapBoth,
						         g_dbg_mapBoth ? "(map mode draws the map on both screens)" : "(map mode leaves the map bottom-only)");
						N3DS_DebugLog(msg);
					}
				}
				else if (strncmp(word, "amnesia", 7) == 0)
				{
#ifdef CTR_NATIVE_OTWALK_PROFILE
					extern int g_dbg_amnesia;

					/* Cache-collapse probe: arms the [AMNESIA] episode logger
					 * in the OT-walk fold (profile builds only). */
					g_dbg_amnesia ^= 1;
					N3DS_DebugLog(g_dbg_amnesia ? "[CTR-3DS] amnesia probe: ON ([AMNESIA] episodes to the instrument log)"
					                            : "[CTR-3DS] amnesia probe: off");
#else
					N3DS_DebugLog("[CTR-3DS] amnesia probe: not in this build (needs CTR_NATIVE_OTWALK_PROFILE)");
#endif
				}
				else if (strncmp(word, "uipop", 5) == 0)
				{
					/* How far UI pops out of the screen, as a percent of this
					 * eye's at-infinity shift. 0 = the old flat-at-screen-plane
					 * behaviour. 'uipop N' sets it directly -- a comfort value only
					 * its owner's eyes can judge, so it is dialled at runtime on
					 * hardware rather than rebuilt. Only visible with the 3D slider
					 * up; identically 0 in mono by construction. */
					const char *arg = word + 5;

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						g_dbg_uiPopPct = atoi(arg);
						if (g_dbg_uiPopPct > 200)
						{
							g_dbg_uiPopPct = 200;
						}
					}
					{
						char msg[96];

						snprintf(msg, sizeof(msg), "[CTR-3DS] ui pop-out: %d%% of k", g_dbg_uiPopPct);
						N3DS_DebugLog(msg);
					}
				}
				else if (strncmp(word, "rainscale", 9) == 0)
				{
					/* Rain-density dial (RenderWeather): 'rainscale N' scales the
					 * per-frame rain particle count to N percent of full retail
					 * density; bare or out-of-range resets to 100. A confirmation
					 * instrument for the Tiger-Temple rain cost -- 'rainscale 0'
					 * is rain OFF, so a pre-launch A/B proves whether the rain is
					 * the shortfall. Thinning the rain is NOT the intended fix; a
					 * batched native rain path that keeps full density is. */
					extern int N3DS_WeatherSetRainScale(int pct);
					const char *arg = word + 9;
					int pct = -1;
					char msg[80];

					while ((*arg == ' ') || (*arg == '\t'))
					{
						arg++;
					}
					if ((*arg >= '0') && (*arg <= '9'))
					{
						pct = atoi(arg);
					}
					pct = N3DS_WeatherSetRainScale(pct);
					snprintf(msg, sizeof(msg), "[CTR-3DS] rain density: %d%% (0 = rain off, 100 = full)", pct);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "spikes", 6) == 0)
				{
					/* Spike catcher (native_perf.c): flush the buffered
					 * dropped-frame records to sdmc:/ctr_spikes.log now. The ring
					 * also auto-flushes on every level change, so a race's spikes
					 * land when you return to the menu -- this is just the manual
					 * grab (e.g. from a pause) with the SD log OFF. */
					extern void NativePerf_DumpSpikesNow(void);

					NativePerf_DumpSpikesNow();
					N3DS_DebugLog("[CTR-3DS] spike catcher: buffer flushed to sdmc:/ctr_spikes.log");
				}
				else if (strncmp(word, "fbtest", 6) == 0)
				{
					extern int g_dbg_skipFbTriTest;

					/* Ablate resolve's per-triangle fb-mirror
					 * overlap test. ON = pause/wipe freeze frames may be
					 * stale; instrument only, see the declaration comment. */
					g_dbg_skipFbTriTest ^= 1;
					N3DS_DebugLog(g_dbg_skipFbTriTest ? "[CTR-3DS] fb tri test: SKIPPED (freeze frames may be stale)" : "[CTR-3DS] fb tri test: on");
				}
				else if (strncmp(word, "otz", 3) == 0)
				{
					extern int g_dbg_otzWiden;

					/* Track-renderer rung 1 v2: RECORD-ONLY bucket->ordinal
					 * table (stamps untouched; v1's restamp was falsified by
					 * its own [OTZ] line). [OTZ2] validation line while on. */
					g_dbg_otzWiden ^= 1;
					N3DS_DebugLog(g_dbg_otzWiden ? "[CTR-3DS] otz table: RECORDING (stamps unchanged)" : "[CTR-3DS] otz table: off");
				}
				else if (strncmp(word, "atlasview", 9) == 0)
				{
					extern int g_dbg_atlasView;
					char msg[48];

					/* 'atlasview N' selects atlas N directly; bare
					 * 'atlasview' cycles off -> 0 -> 1 -> ... -> off. */
					if (word[9] == ' ')
					{
						g_dbg_atlasView = atoi(&word[10]) + 1;
					}
					else
					{
						g_dbg_atlasView = (g_dbg_atlasView + 1) % (N3DS_ATLAS_COUNT + 1);
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] atlas view: %d", g_dbg_atlasView - 1);
					N3DS_DebugLog(g_dbg_atlasView ? msg : "[CTR-3DS] atlas view: off");
				}
				else if (strncmp(word, "verbose", 7) == 0)
				{
					extern int g_dbg_statsVerbose;

					g_dbg_statsVerbose ^= 1;
					N3DS_DebugLog(g_dbg_statsVerbose ? "[CTR-3DS] stats: VERBOSE" : "[CTR-3DS] stats: quiet");
				}
				else if (strncmp(word, "pktbias", 7) == 0)
				{
					extern int g_dbg_pktDepthBias;
					char msg[64];

					/* 'pktbias N' sets an exact value (view.z units nearer);
					 * bare 'pktbias' steps the same preset ladder the hardware
					 * chord uses, so the two controls can't disagree. */
					if (word[7] == ' ')
					{
						g_dbg_pktDepthBias = atoi(&word[8]);
					}
					else
					{
						g_dbg_pktDepthBias = N3DS_NextPktBias(g_dbg_pktDepthBias);
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] packet depth bias: %d (units nearer)", g_dbg_pktDepthBias);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "decalbias", 9) == 0)
				{
					extern int g_dbg_decalNearBias;
					char msg[80];

					/* 'decalbias N': ground-decal (shadow/skid/tire/particle)
					 * nearer bias in view.z units, applied at the capture store
					 * inside the fixed-shift bracket (native_gte_capture.c). Bare
					 * 'decalbias' toggles 0 <-> 32 for a one-token A/B. */
					if (word[9] == ' ')
					{
						g_dbg_decalNearBias = atoi(&word[10]);
					}
					else
					{
						g_dbg_decalNearBias = g_dbg_decalNearBias ? 0 : 32;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] ground-decal nearer bias: %d view.z units", g_dbg_decalNearBias);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "trkchk", 6) == 0)
				{
					/* s32: runs BOTH depth formulas -- the integer one this
					 * project has been validating, and the composed-row float24
					 * one the GPU actually runs -- against retail's GTE for the
					 * same vertex. Taxed (three calls per retail track triangle),
					 * so it is a toggle, not a default. Read [TRKDEPTHCHK] and
					 * [TRKDEPTHSH] together; the gap between them is the point. */
					extern int g_dbg_trackDepthCheck;

					g_dbg_trackDepthCheck ^= 1;
					N3DS_DebugLog(g_dbg_trackDepthCheck ? "[CTR-3DS] track depth check: ON (integer + shader arms)"
					                                    : "[CTR-3DS] track depth check: off");
				}
				else if (strncmp(word, "nofx", 4) == 0)
				{
					extern int g_dbg_noFx;

					g_dbg_noFx ^= 1;
					N3DS_DebugLog(g_dbg_noFx ? "[CTR-3DS] blended packet effects: DROPPED (judging aid)"
					                         : "[CTR-3DS] blended packet effects: drawn");
				}
				else if (strncmp(word, "trknear", 7) == 0)
				{
					/* s32: [TRKNEAR] walks OUR OWN resident mesh and reports its
					 * viewZ range. [TRKDEPTHCHK] cannot answer this -- it only
					 * ever sees vertices RETAIL projects, which is a different
					 * (and possibly much farther) set than the mesh the track
					 * shader actually draws. Read [TRKNEAR] viewZ against
					 * [PKTDEPTHSZ] mdlSz: that pair is the kart's distance and
					 * the road's distance, from the two things that really draw. */
					extern int g_dbg_trackNearCensus;

					g_dbg_trackNearCensus ^= 1;
					N3DS_DebugLog(g_dbg_trackNearCensus ? "[CTR-3DS] track near census: ON" : "[CTR-3DS] track near census: off");
				}
				/* Stereo tokens. ORDER MATTERS within this family: "stereo" is
				 * a prefix of all the others, so the longer tokens must be
				 * tested first or `stereosep 8` would just flip the master
				 * toggle. */
				else if (strncmp(word, "stereocopy", 10) == 0)
				{
					g_dbg_stereoCopy = (word[10] == ' ') ? (atoi(&word[11]) != 0) : !g_dbg_stereoCopy;
					N3DS_DebugLog(g_dbg_stereoCopy ? "[CTR-3DS] stereo right eye: copy of the left eye's command list" : "[CTR-3DS] stereo right eye: journal replay");
				}
				else if (strncmp(word, "stereowindow", 12) == 0)
				{
					g_dbg_stereoWindow ^= 1;
					N3DS_DebugLog(g_dbg_stereoWindow ? "[CTR-3DS] stereo window mode: ON (no pop-out, world starts at the screen)"
					                                 : "[CTR-3DS] stereo window mode: off (pop-out allowed, clamped)");
				}
				else if (strncmp(word, "stereopop", 9) == 0)
				{
					char msg[64];

					/* 'stereopop N' sets the pop-out clamp as a multiple of
					 * the at-infinity shift (0 pins near content to the
					 * screen, 65536 = uncapped). Bare token resets the
					 * default. SELECT+D-LEFT toggles this same value between
					 * capped and uncapped mid-race. */
					if (word[9] == ' ')
					{
						int n = atoi(&word[10]);

						s_stereoPopMul = (float)((n < 0) ? 0 : (n > 65536) ? 65536 : n);
					}
					else
					{
						s_stereoPopMul = 2.0f;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] stereo pop-out cap: %dx at-infinity shift", (int)s_stereoPopMul);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "stereosep", 9) == 0)
				{
					char msg[64];

					/* 'stereosep N' sets the full-slider disparity at infinity
					 * (PSX px). Negative flips the eyes -- the cheap A/B if the
					 * depth sense ever reads inverted on hardware. Bare token
					 * resets the default. */
					if (word[9] == ' ')
					{
						int n = atoi(&word[10]);

						s_stereoSep = (float)((n < -64) ? -64 : (n > 64) ? 64 : n);
					}
					else
					{
						s_stereoSep = 12.0f;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] stereo separation: %d px @ full slider", (int)s_stereoSep);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "stereoconv", 10) == 0)
				{
					char msg[64];

					/* 'stereoconv N' sets the screen-plane depth in view.z
					 * units; nearer content pops out, farther recedes. Bare
					 * token resets the default. */
					if (word[10] == ' ')
					{
						int n = atoi(&word[11]);

						s_stereoConv = (float)((n < 64) ? 64 : (n > 60000) ? 60000 : n);
					}
					else
					{
						s_stereoConv = 1200.0f;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] stereo convergence: %d view.z units", (int)s_stereoConv);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "stereo", 6) == 0)
				{
					g_dbg_stereo ^= 1;
					N3DS_DebugLog(g_dbg_stereo ? "[CTR-3DS] stereo: enabled (slider gates per frame)" : "[CTR-3DS] stereo: FORCED OFF");
				}
				else if (strncmp(word, "depthtest", 9) == 0)
				{
					/* No extern: g_dbg_depthTest lives in THIS translation
					 * unit (see its comment above N3DS_C3DBeginFrame), unlike
					 * the g_dbg_* below that live in the main.c unity TU. */
					static const char *const names[3] = {"NORMAL (LEQUAL + writes)", "NOTEST (painter's order)",
					                                     "TRACKRO (track tests but does not write depth)"};
					char msg[80];

					if (word[9] == ' ')
					{
						int n = atoi(&word[10]);

						g_dbg_depthTest = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_depthTest = (g_dbg_depthTest + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] depth test: %s", names[g_dbg_depthTest]);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "trackrender", 11) == 0)
				{
					extern int g_dbg_trackRender;
					static const char *const names[3] = {"OFF (retail)", "ON (resident replaces retail)", "BOTH (resident + retail overlay)"};
					char msg[64];

					/* 'trackrender N' sets a specific state (clamped 0-2);
					 * bare 'trackrender' cycles 0->1->2->0 -- same idiom as
					 * 'atlasview' above. */
					if (word[11] == ' ')
					{
						int n = atoi(&word[12]);

						g_dbg_trackRender = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_trackRender = (g_dbg_trackRender + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] track render: %s", names[g_dbg_trackRender]);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "modelrender", 11) == 0)
				{
					extern int g_dbg_modelRender;
					static const char *const names[3] = {"OFF (packets only)", "ON (PICA-native models)", "BOTH (native + packet overlay)"};
					char msg[64];

					/* Same idiom as 'trackrender' directly above: 'modelrender N'
					 * sets a state, bare 'modelrender' cycles. State 1 REPLACES
					 * the packet models (RenderBucket_DrawFunc_Normal's
					 * mdlReplaces skips the packet write); state 2 draws both,
					 * as an overlay A/B. A previous version of this comment said
					 * suppression wasn't in yet -- it is, and that stale text
					 * misdiagnosed a bug. */
					if (word[11] == ' ')
					{
						int n = atoi(&word[12]);

						g_dbg_modelRender = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_modelRender = (g_dbg_modelRender + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] model render: %s", names[g_dbg_modelRender]);
					N3DS_DebugLog(msg);
				}
				else if (strncmp(word, "flagstyle", 9) == 0)
				{
					extern int g_dbg_flagStyle;
					static const char *const flagStyleNames[4] = {"retail grey checkers", "PRIDE stripes", "PURPLE/WHITE checkers (Spyro)",
					                                              "BLUE checkers (Retro-Fueled)"};
					char styleMsg[80];

					/* Menu/loading flag style. 'flagstyle N' sets, bare token
					 * cycles 0->1->2->3->0. The non-retail styles are also
					 * reachable in-game by their cheat codes (WONDER, SOAR, ARROW). */
					if (word[9] == ' ')
					{
						const int n = atoi(&word[10]);

						g_dbg_flagStyle = ((n >= 0) && (n <= 3)) ? n : 0;
					}
					else
					{
						g_dbg_flagStyle = (g_dbg_flagStyle + 1) % 4;
					}
					snprintf(styleMsg, sizeof(styleMsg), "[CTR-3DS] race flag: %s", flagStyleNames[g_dbg_flagStyle]);
					N3DS_DebugLog(styleMsg);
				}
				else if (strncmp(word, "pentapal", 8) == 0)
				{
					extern int g_pentaPal;

					/* Opt-in: Penta Penguin's PAL/Japan max-stats (5/5/5) class instead
					 * of the NTSC-U Turn class. Bare token toggles; 'pentapal N' sets
					 * 0/1. Read at kart birth, so toggle before a race starts. */
					if (word[8] == ' ')
					{
						g_pentaPal = (atoi(&word[9]) != 0) ? 1 : 0;
					}
					else
					{
						g_pentaPal = !g_pentaPal;
					}
					N3DS_DebugLog(g_pentaPal ? "[CTR-3DS] Penta: PAL 5/5/5 max class ON" : "[CTR-3DS] Penta: OFF (retail NTSC-U Turn class)");
				}
				else if (strncmp(word, "mirror", 6) == 0)
				{
					extern int g_dbg_mirrorUnlock;

					/* Mirror mode counts as unlocked (the MIRROR row shows on the main menu) without
					 * the four ULTRA HARD cups; the card is not touched. Bare token toggles;
					 * 'mirror N' sets 0/1. */
					if (word[6] == ' ')
					{
						g_dbg_mirrorUnlock = (atoi(&word[7]) != 0) ? 1 : 0;
					}
					else
					{
						g_dbg_mirrorUnlock = !g_dbg_mirrorUnlock;
					}
					N3DS_DebugLog(g_dbg_mirrorUnlock ? "[CTR-3DS] mirror mode: test unlock ON" : "[CTR-3DS] mirror mode: test unlock OFF");
				}
				else if (strncmp(word, "reserves", 8) == 0)
				{
					extern int g_reservesMeter;

					/* Boost-reserves HUD meter by the retail slide bar (DEFAULT ON).
					 * Bare token toggles; 'reserves N' sets 0/1. */
					if (word[8] == ' ')
					{
						g_reservesMeter = (atoi(&word[9]) != 0) ? 1 : 0;
					}
					else
					{
						g_reservesMeter = !g_reservesMeter;
					}
					N3DS_DebugLog(g_reservesMeter ? "[CTR-3DS] reserves meter ON" : "[CTR-3DS] reserves meter OFF");
				}
				else if (strncmp(word, "hudshine", 8) == 0)
				{
					extern int g_dbg_hudShine;

					/* Juiced-wumpa HUD shine in the UI band (DEFAULT ON,
					 * native_gpu.c PacketDepth_ResolveXY). OFF leaves it on the
					 * world-bucket fallback: no widescreen ramp, no stereo
					 * flatten, no far-edge inset. */
					if (word[8] == ' ')
					{
						g_dbg_hudShine = (atoi(&word[9]) != 0) ? 1 : 0;
					}
					else
					{
						g_dbg_hudShine = !g_dbg_hudShine;
					}
					N3DS_DebugLog(g_dbg_hudShine ? "[CTR-3DS] hudshine: ON (wumpa shine in the UI band)"
					                             : "[CTR-3DS] hudshine: off (shine on the world-bucket fallback)");
				}
				else if (strncmp(word, "screenfade", 10) == 0)
				{
					extern int g_dbg_screenFade;

					/* Per-player full-window fade (hit / finish-line flash) in
					 * the UI band (DEFAULT ON, native_gpu.c
					 * PacketDepth_ResolveXY). OFF leaves it on the world-bucket
					 * fallback: depth-tested against the world, 4:3-wide. */
					if (word[10] == ' ')
					{
						g_dbg_screenFade = (atoi(&word[11]) != 0) ? 1 : 0;
					}
					else
					{
						g_dbg_screenFade = !g_dbg_screenFade;
					}
					N3DS_DebugLog(g_dbg_screenFade ? "[CTR-3DS] screenfade: ON (fade quad in the UI band)"
					                               : "[CTR-3DS] screenfade: off (fade quad on the world-bucket fallback)");
				}
				else if (strncmp(word, "tirefix", 7) == 0)
				{
					extern int g_tireReflectFix;

					/* Fix of retail's mirrored-tire delay-slot bug (DEFAULT ON) --
					 * OFF restores the shipped PS1 centerline tires for A/B.
					 * Bare token toggles; 'tirefix N' sets 0/1. */
					if (word[7] == ' ')
					{
						g_tireReflectFix = (atoi(&word[8]) != 0) ? 1 : 0;
					}
					else
					{
						g_tireReflectFix = !g_tireReflectFix;
					}
					N3DS_DebugLog(g_tireReflectFix ? "[CTR-3DS] tire reflection fix ON (wheels at corners)" : "[CTR-3DS] tire reflection fix OFF (retail centerline bug)");
				}
				else if (strncmp(word, "skygpu", 6) == 0)
				{
					extern int g_dbg_skyGpu;

					g_dbg_skyGpu = (word[6] == ' ') ? (atoi(&word[7]) != 0) : !g_dbg_skyGpu;
					N3DS_DebugLog(g_dbg_skyGpu ? "[CTR-3DS] sky dome: GPU" : "[CTR-3DS] sky dome: retail packets");
				}
				else if (strncmp(word, "packetdepth", 11) == 0)
				{
					extern int g_dbg_packetDepth;

					g_dbg_packetDepth ^= 1;
					N3DS_DebugLog(g_dbg_packetDepth ? "[CTR-3DS] packet depth: ON (sz3 true-depth axis)" : "[CTR-3DS] packet depth: off (emit ordinals)");
				}
				else if (strncmp(word, "gtecapture", 10) == 0)
				{
					extern int g_dbg_gteCapture;
					static const char *const names[3] = {"OFF (ablated)", "FULL (store + matrix note)", "STORE ONLY (no matrix note)"};
					char msg[64];

					/* 'gtecapture N' sets a state; bare token cycles 1->2->0. */
					if (word[10] == ' ')
					{
						int n = atoi(&word[11]);

						g_dbg_gteCapture = (n < 0) ? 0 : (n > 2) ? 2 : n;
					}
					else
					{
						g_dbg_gteCapture = (g_dbg_gteCapture + 1) % 3;
					}
					snprintf(msg, sizeof(msg), "[CTR-3DS] gte capture: %s", names[g_dbg_gteCapture]);
					N3DS_DebugLog(msg);
				}
				/* s32 sinking-racers A/B trio. All three default off, so the
				 * first thing on screen is today's behaviour, and each removes
				 * exactly one suspect -- see their definitions in native_gpu.c
				 * and native_gte_capture.c. Read [PKTDEPTH2] alongside. */
				else if ((strncmp(word, "watch", 5) == 0) || (strncmp(word, "hide", 4) == 0))
				{
					extern char g_dbg_watchName[16];
					extern int g_dbg_watchHide;
					char msg[64];
					int hide = (word[0] == 'h');
					const char *arg = word + (hide ? 4 : 5);

					/* 'watch <name>' taps one named object (instance name
					 * OR model name, prefix match -- e.g. 'watch prize1',
					 * 'watch BOX'); [WATCH] lines report its store-side depth
					 * story while it draws. 'hide <name>' also skips its draw.
					 * Bare 'watch'/'hide' turns both off. */
					g_dbg_watchHide = hide;
					if (arg[0] == ' ')
					{
						int wi;

						for (wi = 0; (wi < 15) && (arg[1 + wi] != '\0') && (arg[1 + wi] != '\n') && (arg[1 + wi] != '\r') && (arg[1 + wi] != ' '); wi++)
						{
							g_dbg_watchName[wi] = arg[1 + wi];
						}
						g_dbg_watchName[wi] = '\0';
						snprintf(msg, sizeof(msg), "[CTR-3DS] watch: '%s' ([WATCH] while it draws)", g_dbg_watchName);
						N3DS_DebugLog(msg);
					}
					else
					{
						g_dbg_watchName[0] = '\0';
						N3DS_DebugLog("[CTR-3DS] watch: off");
					}
				}
				else if (strncmp(word, "ontop", 5) == 0)
				{
					extern int g_dbg_paintOnTop;

					/* Paint-on-top model class (talking-mask hint heads):
					 * occlusion pinned to the nearest world stamp, parallax
					 * honest -- retail parks them inside hub geometry and its
					 * draw order carries them; our depth test needs the
					 * explicit statement. Allowlist: native_gte_capture.c. */
					g_dbg_paintOnTop ^= 1;
					N3DS_DebugLog(g_dbg_paintOnTop ? "[CTR-3DS] paint-on-top models: ON (masks win the depth test)"
					                               : "[CTR-3DS] paint-on-top models: off (buried-in-ground A/B leg)");
				}
				else if (strncmp(word, "under", 5) == 0)
				{
					extern int g_dbg_paintUnder;

					/* Paint-under class (intro crate's BOX_02A shell): packet
					 * path, captured depth biased farther. Allowlist:
					 * native_gte_capture.c. */
					g_dbg_paintUnder ^= 1;
					N3DS_DebugLog(g_dbg_paintUnder ? "[CTR-3DS] paint-under members: ON (hidden inside their cover)"
					                               : "[CTR-3DS] paint-under members: off (flat-orange crate A/B leg)");
				}
				else
				{
					N3DS_DebugCaptureAll();
				}
			}
		}
	}

	/* SELECT held + R: SD logging on/off (g_dbg_sdLog, defined by the log
	 * statics near the top of this file). The toggle the owner reaches for
	 * most -- ON costs a ~100-170ms hitch every 2s, so it is the one switch
	 * whose state can invalidate a whole sitting. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_R))
	{
		g_dbg_sdLog ^= 1;
		/* Logged AFTER the flip so turning it back ON announces itself in the
		 * file; turning it off leaves its last word in the svc trail only. */
		N3DS_DebugLog(g_dbg_sdLog ? "[CTR-3DS] sdlog: ON (writing ctr3ds.log -- costs a ~2s hitch)"
		                          : "[CTR-3DS] sdlog: off (no SD writes; bottom screen still live)");
	}

	/* The chord map is deliberately this small -- owner's ruling, the chords
	 * below are the ones he actually reaches for. Everything else is a dumpreq
	 * token only: `verbose` (bottom-screen stats),
	 * `depthtest [N]`, `padmux [N]`, `gtecapture [N]`,
	 * `packetdepth`, `trackrender`, `modelrender`, `wide`.
	 * B / X / Y / ZL / ZR / D-UP / D-DOWN / A / L are all free. A control
	 * earns a chord only when it must flip mid-race (the game holds the SD
	 * card then, so dumpreq is unplaceable) AND the owner will use it;
	 * otherwise bind nothing and grep this file first -- a colliding chord
	 * once confounded two A/Bs at once. */

	/* SELECT+DRIGHT: track-atlas sampler A/B, nearest <-> bilinear. Pure
	 * mid-race flip -- rewrites each unit's filter field, no re-upload, no
	 * rebuild -- so it lands the same frame. dumpreq `bilinear` is the same
	 * path for Azahar. (Was the textureless debug view, now dumpreq-only.) */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_DRIGHT))
	{
		g_dbg_trackBilinear ^= 1;
		N3DS_TrackAtlas_ApplyFilter();
		N3DS_DebugLog(g_dbg_trackBilinear ? "[CTR-3DS] track filter: BILINEAR" : "[CTR-3DS] track filter: nearest");
	}

	/* SELECT held + D-pad LEFT: stereo pop-out cap <-> uncapped. A button
	 * because the question is whether uncapped pop-out survives normal racing,
	 * which needs eyes on the 3D screen mid-race with the slider in hand --
	 * dumpreq is unplaceable while the game holds the SD card. Uncapping
	 * remembers the capped value it left, so a dumpreq-tuned cap (`stereopop
	 * N`) round-trips through the toggle. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_DLEFT))
	{
		static float s_popMulCapped = 2.0f;

		if (s_stereoPopMul < N3DS_STEREO_POP_UNCAPPED)
		{
			s_popMulCapped = s_stereoPopMul;
			s_stereoPopMul = N3DS_STEREO_POP_UNCAPPED;
			N3DS_DebugLog("[CTR-3DS] stereo pop-out: UNCAPPED");
		}
		else
		{
			char msg[64];

			s_stereoPopMul = s_popMulCapped;
			snprintf(msg, sizeof(msg), "[CTR-3DS] stereo pop-out cap: %dx at-infinity shift", (int)s_stereoPopMul);
			N3DS_DebugLog(msg);
		}
	}

	/* SELECT held + START: the owner's "our config" combo --
	 * track renderer + native models + packet depth all flip together, because
	 * reaching the target config from a clean boot cost three chords every
	 * boot. Two states: everything native (1,1,1) or everything retail
	 * (0,0,0); a mixed state goes native first, so one press from clean boot
	 * lands on the config being targeted. START still reaches the game, so
	 * in-race this also opens the pause menu -- acceptable and documented.
	 *
	 * This is now the ONLY renderer chord. The individual track / models /
	 * packet-depth buttons (ZR / A / L) and the widescreen button (D-DOWN) are
	 * gone -- owner's call, "not much use in individual toggles, all in one, I
	 * don't wanna fiddle with toggles". Every one of them survives as a dumpreq
	 * token (`trackrender`, `modelrender`, `packetdepth`, `wide`) for the day a
	 * single variable genuinely needs isolating, and isolating one variable is
	 * a deliberate debugging act that can afford a relaunch. Widescreen joins
	 * the master because "ours" includes it: REAL (2) on, 4:3 crop (0) off; the
	 * rejected stretch (1) stays dumpreq-only. */
	if ((held & KEY_SELECT) && (hidKeysDown() & KEY_START))
	{
		extern int g_dbg_trackRender;
		extern int g_dbg_modelRender;
		extern int g_dbg_packetDepth;
		const int allOn =
		    (g_dbg_trackRender != 0) && (g_dbg_modelRender != 0) && (g_dbg_packetDepth != 0) && (g_dbg_widescreen != 0);
		const int target = allOn ? 0 : 1;

		g_dbg_trackRender = target;
		g_dbg_modelRender = target;
		g_dbg_packetDepth = target;
		g_dbg_widescreen = target ? 2 : 0; /* REAL, not stretch */
		N3DS_DebugLog(target ? "[CTR-3DS] OUR CONFIG: track+models+dmodel+widescreen all ON"
		                     : "[CTR-3DS] our config: all off (full retail, 4:3)");
	}

#endif

	if (held & KEY_A)
	{
		out |= N3DS_BTN_A;
	}
	if (held & KEY_B)
	{
		out |= N3DS_BTN_B;
	}
	if (held & KEY_X)
	{
		out |= N3DS_BTN_X;
	}
	if (held & KEY_Y)
	{
		out |= N3DS_BTN_Y;
	}
	if (held & KEY_L)
	{
		out |= N3DS_BTN_L;
	}
	if (held & KEY_R)
	{
		out |= N3DS_BTN_R;
	}
	if (held & KEY_ZL)
	{
		out |= N3DS_BTN_ZL;
	}
	if (held & KEY_ZR)
	{
		out |= N3DS_BTN_ZR;
	}
	if (held & KEY_START)
	{
		out |= N3DS_BTN_START;
	}
	if (held & KEY_SELECT)
	{
		out |= N3DS_BTN_SELECT;
	}
	if (held & (KEY_DUP | KEY_CPAD_UP))
	{
		out |= N3DS_BTN_UP;
	}
	if (held & (KEY_DDOWN | KEY_CPAD_DOWN))
	{
		out |= N3DS_BTN_DOWN;
	}
	if (held & (KEY_DLEFT | KEY_CPAD_LEFT))
	{
		out |= N3DS_BTN_LEFT;
	}
	if (held & (KEY_DRIGHT | KEY_CPAD_RIGHT))
	{
		out |= N3DS_BTN_RIGHT;
	}

	return out;
}

unsigned long N3DS_HeapUsedBytes(void)
{
	struct mallinfo mi = mallinfo();

	return (unsigned long)mi.uordblks;
}

unsigned long N3DS_LinearFreeBytes(void)
{
	return (unsigned long)linearSpaceFree();
}

void N3DS_LogMemory(const char *where)
{
	struct mallinfo mi = mallinfo();
	char line[160];

	snprintf(line, sizeof(line), "[CTR-3DS] mem %s: heap %lu KiB used of %lu, linear %lu KiB free of %lu, new3ds=%d", where,
	         (unsigned long)mi.uordblks / 1024UL, (unsigned long)envGetHeapSize() / 1024UL, (unsigned long)linearSpaceFree() / 1024UL,
	         (unsigned long)envGetLinearHeapSize() / 1024UL, N3DS_IsNew3DS());
	N3DS_DebugLog(line);
	/* Committed to the card at once: the line after it may be the allocation that fails. Boot and load only, never a timed frame. */
	N3DS_LogFlushNow();
}

/*
 * Once-per-frame main-thread heartbeat, thaw detector, and light watchdog
 * (Rosalina-freeze hardening). Called exactly
 * once per frame regardless of render-thread routing, so it is the one place
 * guaranteed to run on the game thread every frame.
 */
static void N3DS_BeatMainTick(void)
{
	static u64 s_beatLastTick;
	static unsigned int s_beatFrameCount;
	static unsigned int s_beatPrevAudioCounter;
	static unsigned int s_beatPrevRwCounter;
	static u64 s_beatMainStallStartTick;
	static unsigned int s_beatPostThawFrames;
	const u64 now = svcGetSystemTick();

	if (s_beatLastTick != 0)
	{
		const u64 gapMs = ((now - s_beatLastTick) * 1000ull) / SYSCLOCK_ARM11;

		/* A gap this large cannot be ordinary frame variance -- it is the
		 * signature of a Rosalina freeze/thaw (or HOME/lid suspend) that just
		 * released this thread. Re-arm the boot-time frame-bracket trace so
		 * the next few C3D_FrameBegin/End calls narrate themselves, the same
		 * way the first few frames of boot do. */
		if (gapMs > 500)
		{
			char msg[96];

			snprintf(msg, sizeof(msg), "[BEAT] THAW gapMs=%u vb0=%lu vb1=%lu", (unsigned)gapMs, (unsigned long)C3D_FrameCounter(0),
			         (unsigned long)C3D_FrameCounter(1));
			N3DS_DebugLog(msg);
			s_frameTraceBudget = 64;
			s_beatPostThawFrames = 600;
			N3DS_LogFlushNow();
		}
	}
	s_beatLastTick = now;

	N3DS_BeatMark(N3DS_BEAT_THREAD_MAIN, N3DS_BEAT_WHERE_OK);
	s_beatFrameCount++;

	/* Post-thaw window: prove the game thread keeps turning after a freeze
	 * even when nothing reaches the screens -- one committed line every 64
	 * frames for ~10s, then back to the normal cadence. */
	if (s_beatPostThawFrames > 0)
	{
		s_beatPostThawFrames--;
		if ((s_beatPostThawFrames % 64) == 0)
		{
			char msg[96];

			snprintf(msg, sizeof(msg), "[BEAT] ALIVE post-thaw main=%u vb0=%lu vb1=%lu", s_beatFrameCount,
			         (unsigned long)C3D_FrameCounter(0), (unsigned long)C3D_FrameCounter(1));
			N3DS_DebugLog(msg);
			N3DS_LogFlushNow();
		}
	}

	if ((s_beatFrameCount % 256) == 0)
	{
		unsigned int audioCounter = 0;
		int audioWhere = N3DS_BEAT_WHERE_OK;
		unsigned int rwCounter = 0;
		int rwWhere = N3DS_BEAT_WHERE_OK;
		int audioStuck;
		int rwStuck;

		N3DS_BeatRead(N3DS_BEAT_THREAD_AUDIO, &audioCounter, &audioWhere);
		/* Either worker may legitimately not exist (Old3DS, spawn failure,
		 * or the 'renderthread' A/B switched off) -- its counter then sits
		 * at 0 forever, which must never itself read as a stall. */
		N3DS_BeatRead(N3DS_BEAT_THREAD_RENDERWORKER, &rwCounter, &rwWhere);

		audioStuck = (audioCounter != 0) && (audioCounter == s_beatPrevAudioCounter);
		rwStuck = (rwCounter != 0) && (rwCounter == s_beatPrevRwCounter);

		if (audioStuck || rwStuck)
		{
			char msg[128];
			unsigned int ageMs;

			if (s_beatMainStallStartTick == 0)
			{
				s_beatMainStallStartTick = now;
			}
			ageMs = (unsigned int)(((now - s_beatMainStallStartTick) * 1000ull) / SYSCLOCK_ARM11);

			snprintf(msg, sizeof(msg), "[BEAT] STALL aw=%u@%s rw=%u@%s main=%u ageMs=%u", audioCounter, N3DS_BeatWhereName(audioWhere),
			         rwCounter, N3DS_BeatWhereName(rwWhere), s_beatFrameCount, ageMs);
			N3DS_DebugLog(msg);
		}
		else
		{
			s_beatMainStallStartTick = 0;
		}

		{
			char msg[64];

			snprintf(msg, sizeof(msg), "[BEAT] main=%u aw=%u rw=%u", s_beatFrameCount, audioCounter, rwCounter);
			N3DS_DebugLog(msg);
		}

		s_beatPrevAudioCounter = audioCounter;
		s_beatPrevRwCounter = rwCounter;
	}
}

int N3DS_MainLoopActive(void)
{
	N3DS_BeatMainTick();
	return aptMainLoop() ? 1 : 0;
}
