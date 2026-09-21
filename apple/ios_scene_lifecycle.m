// Makes this app adopt UIKit's scene-based life cycle, which SDL2 does not.
//
// Apps built against the iOS 26 SDK and later must adopt UIScene or they are
// killed at launch:
//
//   Application failed to launch: UIScene life cycle is required for apps
//   built with this SDK.
//
// There is no opting out by lowering the deployment target -- the rule is
// about the SDK the binary was built against, not the OS it runs on. And
// there is no fix coming from SDL2: scene support landed in SDL3 and was not
// backported. So it is done here, in the app, rather than by carrying a patch
// against somebody else's SDL checkout.
//
// Three pieces, and all three are needed:
//
//   1. A category on SDLUIKitDelegate returning a different delegate class
//      name. This is not a trick -- SDL's own source says to do exactly this:
//      "when you subclass this appdelegate, make sure to add a category to
//      override this method and return the actual name of the delegate".
//
//   2. A subclass that answers the scene-session hook. Declaring
//      UIApplicationSceneManifest in Info.plist is not sufficient on its own:
//      the runtime checks that the live application delegate actually responds
//      to the connection hook. The manifest is there too, because Apple
//      documents both halves, and the two agree.
//
//   3. Somewhere for the picture to go. A UIWindow with no windowScene is a
//      window on no display, and SDL creates its windows the old way --
//      [[UIWindow alloc] initWithFrame:] and nothing else. Attaching them is
//      ds_attach_sdl_window_to_scene(), called from gl_window.cpp once the
//      window exists.
//
// Compiled without ARC, deliberately. SDL_SysWMinfo holds its UIWindow in a
// union, and ARC refuses to compile a union containing an object pointer.
// Nothing below owns anything, so manual retain/release costs nothing here.

#import <UIKit/UIKit.h>

#include <stddef.h>

#include <SDL.h>
#include <SDL_syswm.h>

// --- what SDL already has, redeclared ---------------------------------------
//
// SDL publishes no header for any of this: SDL_uikitappdelegate.h lives in
// SDL's source tree and is not installed. These name what is there at runtime
// so the compiler will let it be called and overridden, and they match SDL
// 2.32 exactly. A future SDL that changes them breaks this file at compile
// time, which is the right place to find out.
@interface SDLUIKitDelegate : NSObject <UIApplicationDelegate>
+ (id)sharedAppDelegate;
+ (NSString *)getAppDelegateClassName;
@end

@interface SDLUIKitDelegate (DualscreenLaunchHook)
// SDL's "stop preparing and run SDL_main". Its
// application:didFinishLaunchingWithOptions: schedules this for the next turn
// of the run loop, which may be before any scene has connected -- see below.
- (void)postFinishLaunch;
@end

// --- the two classes --------------------------------------------------------

@interface DualscreenSceneDelegate : UIResponder <UIWindowSceneDelegate>
@end

@interface DualscreenAppDelegate : SDLUIKitDelegate
// Called by the scene delegate once there is somewhere to draw.
- (void)dualscreenSceneConnected;
@end

@implementation DualscreenSceneDelegate

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options
{
    (void)session;
    (void)options;
    if (![scene isKindOfClass:[UIWindowScene class]]) {
        return;
    }
    id delegate = [UIApplication sharedApplication].delegate;
    if ([delegate isKindOfClass:[DualscreenAppDelegate class]]) {
        [(DualscreenAppDelegate *)delegate dualscreenSceneConnected];
    }
}

@end

@implementation DualscreenAppDelegate {
    BOOL _sceneConnected;
    BOOL _mainDeferred;
}

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options
{
    (void)application;
    (void)options;
    UISceneConfiguration *configuration =
        [UISceneConfiguration configurationWithName:@"Dualscreen"
                                        sessionRole:session.role];
    configuration.delegateClass = [DualscreenSceneDelegate class];
    return configuration;
}

// SDL asks to run SDL_main as soon as the application has launched, which is
// not necessarily after a scene has connected. SDL_main creates the window
// almost immediately, and a window made before there is a scene to put it on
// never appears.
//
// So it waits. Nothing is skipped -- super's implementation runs in full, just
// once there is somewhere for it to draw. Either order works: if the scene
// arrives first this never defers at all.
- (void)postFinishLaunch
{
    if (!_sceneConnected) {
        _mainDeferred = YES;
        return;
    }
    [super postFinishLaunch];
}

- (void)dualscreenSceneConnected
{
    if (_sceneConnected) {
        return;
    }
    _sceneConnected = YES;
    if (_mainDeferred) {
        _mainDeferred = NO;
        // Next turn of the run loop rather than right here: SDL_main does not
        // return until the app is quitting, and this is inside scene
        // connection.
        [self performSelector:@selector(postFinishLaunch) withObject:nil afterDelay:0.0];
    }
}

@end

// SDL reads the delegate's name from here and hands it to UIApplicationMain.
//
// A category implementing a method its class already implements is ordinarily
// a mistake, and the compiler says so. Here it is the documented way in --
// SDL's own source says to override this method from a category -- and the
// method exists for no other purpose. Silenced narrowly, over these few lines
// only, so that the warning still means something everywhere else.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"

@implementation SDLUIKitDelegate (DualscreenAppDelegateName)

+ (NSString *)getAppDelegateClassName
{
    return @"DualscreenAppDelegate";
}

@end

#pragma clang diagnostic pop

// --- giving an SDL window somewhere to be ------------------------------------

void ds_attach_sdl_window_to_scene(SDL_Window *window)
{
    if (window == NULL) {
        return;
    }
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(window, &info) != SDL_TRUE) {
        return;
    }
    if (info.subsystem != SDL_SYSWM_UIKIT) {
        return;
    }
    UIWindow *uiwindow = info.info.uikit.window;
    if (uiwindow == nil || uiwindow.windowScene != nil) {
        return;
    }
    for (UIScene *scene in [UIApplication sharedApplication].connectedScenes) {
        if (![scene isKindOfClass:[UIWindowScene class]]) {
            continue;
        }
        uiwindow.windowScene = (UIWindowScene *)scene;
        // SDL already called this once, before the window could be shown on
        // anything. It is idempotent and now it can do something.
        [uiwindow makeKeyAndVisible];
        return;
    }
    SDL_Log("dualscreen: the window has no scene to attach to, so nothing will draw");
}
