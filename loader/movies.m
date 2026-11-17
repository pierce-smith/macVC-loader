// startup movies, the rockstar logo and the vice city titles like on windows.
//


// macVC's startup state goes straight to init once (reVC left movie playback to windows),
// so movies/Logo.mpg and movies/GTAtitles.mpg never show. a midhook on that state change
// holds the game in startup while each movie plays in an AVPlayerLayer over the window,
// and the main loop keeps pumping window events meanwhile.
// enter, space, escape, a click or A/Start skips. PlayMovies=0 in macVCLoader.ini or
// macVC's own no movies option turns them off.
#import <AVFoundation/AVFoundation.h>
#import <AppKit/AppKit.h>
#include <dlfcn.h>
#include <unistd.h>
#include "internal.h"

static AVPlayer *g_player;
static AVPlayerLayer *g_layer;
static id g_end_observer;
static int g_index, g_done, g_ended, g_armed;
static const char *g_files[] = {"movies/Logo.mpg", "movies/GTAtitles.mpg"};

static NSView *game_view(void) {
    static id (*getCocoaWindow)(void *);
    if (!getCocoaWindow) *(void **)&getCocoaWindow = dlsym(RTLD_DEFAULT, "glfwGetCocoaWindow");
    void *win = loader_game_window();
    NSWindow *w = (win && getCocoaWindow) ? getCocoaWindow(win) : nil;
    return w.contentView;
}

static void stop_movie(void) {
    if (g_end_observer) [[NSNotificationCenter defaultCenter] removeObserver:g_end_observer];
    g_end_observer = nil;
    [g_player pause];
    [g_layer removeFromSuperlayer];
    g_player = nil;
    g_layer = nil;
}

static int start_movie(const char *rel) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", g_game_dir, rel);
    if (access(path, R_OK)) return 0;
    NSView *view = game_view();
    if (!view) return 0;
    view.wantsLayer = YES;
    AVPlayerItem *item = [AVPlayerItem playerItemWithURL:[NSURL fileURLWithPath:@(path)]];
    g_player = [AVPlayer playerWithPlayerItem:item];
    g_layer = [AVPlayerLayer playerLayerWithPlayer:g_player];
    g_layer.videoGravity = AVLayerVideoGravityResizeAspect;
    g_layer.backgroundColor = NSColor.blackColor.CGColor;
    g_layer.frame = view.bounds;
    g_layer.zPosition = 10000;
    [view.layer addSublayer:g_layer];
    g_ended = 0;
    g_end_observer = [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                                                                       object:item queue:nil usingBlock:^(NSNotification *n) {
        (void)n;
        g_ended = 1;
    }];
    [g_player play];
    g_armed = 0; // a key still held from the previous movie doesn't skip this one
    mvc_log("[Loader] playing %s", rel);
    return 1;
}

// runs at the start-up state's "gGameState = GS_INIT_ONCE": x8 holds the new state
static void movie_tick(MVCContext *ctx, void *user) {
    (void)user;
    if (g_done) return;
    @autoreleasepool {
        if (!g_player) {
            while (g_index < 2 && !start_movie(g_files[g_index])) g_index++;
            if (!g_player) { g_done = 1; return; } // all played (or missing): carry on starting the game
        }
        NSView *view = game_view();
        if (view) g_layer.frame = view.bounds; // follow window resizes / fullscreen
        int skip = loader_skip_pressed();
        if (!skip) g_armed = 1;
        int failed = g_player.currentItem.status == AVPlayerItemStatusFailed;
        if (failed) mvc_log_warn("[Loader] %s can't be played", g_files[g_index]);
        if (g_ended || failed || (skip && g_armed)) {
            stop_movie();
            g_index++;
        }
    }
    ctx->x[8] = 0; // stay in start-up for another pass of the main loop
    usleep(8000);
}

void movies_init(void) {
    if (!g_cfg.play_movies) return;
    uint8_t *nomovies = mvc_sym("gbNoMovies");
    if (nomovies && *nomovies) return;
    uint32_t *code = mvc_sym("main");
    uintptr_t state = (uintptr_t)mvc_sym("gGameState");
    if (!code || !state) return;
    // find "mov w8, #5 ; str w8, [xN, #gGameState]" followed by the trace text "gGameState = GS_INIT_ONCE"
    for (int i = 0; i < 6000; i++) {
        if (code[i] != 0x528000A8) continue;                                          // mov w8, #5
        uint32_t st = code[i + 1];
        if ((st & 0xFFC0001F) != 0xB9000008 || ((st >> 10) & 0xFFF) * 4 != (state & 0xFFF)) continue; // str w8, [xN, #off]
        int traced = 0;
        uint64_t page = 0;
        for (int k = i + 2; k < i + 12; k++) {
            uint32_t in = code[k];
            if ((in & 0x9F00001F) == 0x90000003) { // adrp x3
                uint64_t raw = (((uint64_t)(in >> 5) & 0x7FFFF) << 2) | ((in >> 29) & 3);
                page = ((uintptr_t)&code[k] & ~0xFFFULL) + (uint64_t)(((int64_t)(raw << 43) >> 43) * 4096);
            } else if ((in & 0xFFC003FF) == 0x91000063 && page) { // add x3, x3, #imm
                const char *s = (const char *)(page + ((in >> 10) & 0xFFF));
                if (!strcmp(s, "gGameState = GS_INIT_ONCE")) traced = 1;
            }
        }
        if (!traced) continue;
        if (mvc_midhook(&code[i + 1], movie_tick, NULL) == 0)
            mvc_log("[Loader] startup movies on (PlayMovies=0 in macVCLoader.ini turns them off)");
        return;
    }
    mvc_log_warn("[Loader] startup movies: this macVC build's start-up code is different - not shown");
}
