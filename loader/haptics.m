// controller rumble through GameController + CoreHaptics. both only get loaded on the
// first rumble, never at launch, so they can't mess with the game's glfw controller code.
#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <dlfcn.h>
#include "internal.h"

static NSMutableDictionary *g_engines;

static id constant(const char *name) {
    void **p = dlsym(RTLD_DEFAULT, name);
    return p ? (__bridge id)*p : nil;
}

int mvc_pad_rumble(int pad, float strength, int ms) {
    @autoreleasepool {
        static int loaded;
        if (!loaded) {
            loaded = dlopen("/System/Library/Frameworks/GameController.framework/GameController", RTLD_LAZY) &&
                     dlopen("/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics", RTLD_LAZY) ? 1 : -1;
            g_engines = [NSMutableDictionary new];
        }
        if (loaded < 0) return 0;
        Class GCC = NSClassFromString(@"GCController");
        NSArray *list = [GCC controllers];
        if (pad < 0 || pad >= (int)list.count) return 0;
        id ctrl = list[(NSUInteger)pad];
        id haptics = [ctrl haptics];
        if (!haptics) return 0;
        NSValue *key = [NSValue valueWithNonretainedObject:ctrl];
        id engine = g_engines[key];
        if (!engine) {
            id locality = constant("GCHapticsLocalityDefault");
            if (!locality) return 0;
            engine = [haptics createEngineWithLocality:locality];
            if (!engine || ![engine startAndReturnError:nil]) return 0;
            g_engines[key] = engine;
        }
        Class Param = NSClassFromString(@"CHHapticEventParameter");
        Class Event = NSClassFromString(@"CHHapticEvent");
        Class Pattern = NSClassFromString(@"CHHapticPattern");
        id intensityID = constant("CHHapticEventParameterIDHapticIntensity");
        id continuous = constant("CHHapticEventTypeHapticContinuous");
        if (!Param || !Event || !Pattern || !intensityID || !continuous) return 0;
        float s = strength < 0 ? 0 : strength > 1 ? 1 : strength;
        id param = [[Param alloc] initWithParameterID:intensityID value:s];
        id ev = [[Event alloc] initWithEventType:continuous parameters:@[param] relativeTime:0 duration:ms / 1000.0];
        id pattern = [[Pattern alloc] initWithEvents:@[ev] parameters:@[] error:nil];
        id player = pattern ? [engine createPlayerWithPattern:pattern error:nil] : nil;
        if (!player) return 0;
        return [player startAtTime:0 error:nil] ? 1 : 0;
    }
}

// game rumble
// the physical controller behind the game's GLFW device. with macOS's
// controller compatibility mode the game sees a synthetic "GamePad-N" while
// GameController still sees the real DualSense / DualShock, haptics included.
static id controller_for(const char *glfw_name) {
    Class GCC = NSClassFromString(@"GCController");
    NSArray *list = [GCC controllers];
    NSString *want = nil;
    if (glfw_name) {
        if (strstr(glfw_name, "DualSense") || !strncmp(glfw_name, "GamePad-", 8)) want = @"DualSense";
        else if (strstr(glfw_name, "DualShock") || strstr(glfw_name, "Wireless Controller")) want = @"DualShock";
        else if (strstr(glfw_name, "Xbox")) want = @"Xbox";
    }
    if (want)
        for (id c in list)
            if ([c haptics] && [[c productCategory] rangeOfString:want options:NSCaseInsensitiveSearch].location != NSNotFound) return c;
    id cur = [GCC current];
    if (cur && [cur haptics]) return cur;
    for (id c in list) if ([c haptics]) return c;
    return nil;
}

static id g_player; // the shake playing now

void haptics_stop(void) {
    @autoreleasepool {
        if (g_player) { [g_player stopAtTime:0 error:nil]; g_player = nil; }
    }
}

int haptics_play(const char *glfw_name, float strength, int ms) {
    @autoreleasepool {
        static int loaded;
        if (!loaded) {
            loaded = dlopen("/System/Library/Frameworks/GameController.framework/GameController", RTLD_LAZY) &&
                     dlopen("/System/Library/Frameworks/CoreHaptics.framework/CoreHaptics", RTLD_LAZY) ? 1 : -1;
            g_engines = [NSMutableDictionary new];
        }
        if (loaded < 0) return 0;
        id ctrl = controller_for(glfw_name);
        if (!ctrl) return 0;
        static id logged;
        if (ctrl != logged) { logged = ctrl; mvc_log("[Loader] rumble goes to %s", [[ctrl productCategory] UTF8String] ?: "a controller"); }
        NSValue *key = [NSValue valueWithNonretainedObject:ctrl];
        id engine = g_engines[key];
        if (!engine) {
            id locality = constant("GCHapticsLocalityDefault");
            if (!locality) return 0;
            engine = [[ctrl haptics] createEngineWithLocality:locality];
            if (!engine || ![engine startAndReturnError:nil]) return 0;
            [engine setStoppedHandler:^(NSInteger reason) { (void)reason; [g_engines removeObjectForKey:key]; }];
            g_engines[key] = engine;
        }
        Class Param = NSClassFromString(@"CHHapticEventParameter");
        Class Event = NSClassFromString(@"CHHapticEvent");
        Class Pattern = NSClassFromString(@"CHHapticPattern");
        id intensityID = constant("CHHapticEventParameterIDHapticIntensity");
        id sharpID = constant("CHHapticEventParameterIDHapticSharpness");
        id continuous = constant("CHHapticEventTypeHapticContinuous");
        if (!Param || !Event || !Pattern || !intensityID || !continuous) return 0;
        float s = strength < 0 ? 0 : strength > 1 ? 1 : strength;
        NSMutableArray *params = [NSMutableArray arrayWithObject:[[Param alloc] initWithParameterID:intensityID value:s]];
        if (sharpID) [params addObject:[[Param alloc] initWithParameterID:sharpID value:0.3f]]; // a rumble, not a click
        id ev = [[Event alloc] initWithEventType:continuous parameters:params relativeTime:0 duration:ms / 1000.0];
        id pattern = [[Pattern alloc] initWithEvents:@[ev] parameters:@[] error:nil];
        id player = pattern ? [engine createPlayerWithPattern:pattern error:nil] : nil;
        if (!player) return 0;
        if (g_player) [g_player stopAtTime:0 error:nil];
        g_player = player;
        return [player startAtTime:0 error:nil] ? 1 : 0;
    }
}
