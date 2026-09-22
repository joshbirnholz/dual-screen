// The slice of UIKit that apple/ios_scene_lifecycle.m names, and nothing else.
//
// That file is Objective-C, it is compiled only for iOS and tvOS, and it is
// the one file here that decides whether the app launches at all. Until this
// stub existed, nothing on this machine had ever parsed it -- a mistyped
// selector or an unbalanced brace would have been found by a person, in
// Xcode, at the end of a build.
//
// These are signatures, not behaviour. Passing proves the file agrees with
// the shape of the API it calls; it says nothing about whether a scene
// actually connects, which only a device or a simulator can answer.
#pragma once

#include <TargetConditionals.h>

typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#ifndef nil
#define nil ((id)0)
#endif

@interface NSObject
+ (Class)class;
- (BOOL)isKindOfClass:(Class)cls;
- (BOOL)respondsToSelector:(SEL)selector;
- (id)performSelector:(SEL)selector;
- (void)performSelector:(SEL)selector withObject:(id)object afterDelay:(double)delay;
@end

@interface NSString : NSObject
@end

typedef struct {
    unsigned long state;
    id *itemsPtr;
    unsigned long *mutationsPtr;
    unsigned long extra[5];
} NSFastEnumerationState;

@protocol NSFastEnumeration
- (unsigned long)countByEnumeratingWithState:(NSFastEnumerationState *)state
                                     objects:(id *)buffer
                                       count:(unsigned long)len;
@end

@interface NSSet : NSObject <NSFastEnumeration>
@end

@class UIApplication;
@class UIScene;
@class UIWindowScene;
@class UISceneSession;
@class UISceneConnectionOptions;

@interface UIResponder : NSObject
@end

@interface UISceneConfiguration : NSObject
+ (instancetype)configurationWithName:(NSString *)name sessionRole:(NSString *)role;
@property (nonatomic, assign) Class delegateClass;
@end

@interface UISceneSession : NSObject
@property (nonatomic, readonly) NSString *role;
@end

@interface UISceneConnectionOptions : NSObject
@end

@interface UIScene : UIResponder
@end

// Orientation does not exist on tvOS. Not "is absent" -- the real SDK marks
// the type and the property explicitly unavailable there, so naming them is a
// compile error and not a missing symbol at link time. Left out here under the
// same condition, because a stub that is more generous than the SDK is a stub
// that passes code the SDK will reject. This one did, once: an orientation
// reader compiled as tvOS here and failed in Xcode.
#if !TARGET_OS_TV
typedef long UIInterfaceOrientation;
#define UIInterfaceOrientationPortrait ((UIInterfaceOrientation)1)
#define UIInterfaceOrientationPortraitUpsideDown ((UIInterfaceOrientation)2)
#define UIInterfaceOrientationLandscapeLeft ((UIInterfaceOrientation)4)
#define UIInterfaceOrientationLandscapeRight ((UIInterfaceOrientation)3)
#endif

@interface UIWindowScene : UIScene
#if !TARGET_OS_TV
@property (nonatomic, readonly) UIInterfaceOrientation interfaceOrientation;
#endif
@end

#if !TARGET_OS_TV
typedef unsigned long UIInterfaceOrientationMask;
#define UIInterfaceOrientationMaskAll ((UIInterfaceOrientationMask)0x1E)
#define UIInterfaceOrientationMaskLandscape ((UIInterfaceOrientationMask)0x18)
#endif

@interface UIViewController : UIResponder
#if !TARGET_OS_TV
- (void)setNeedsUpdateOfSupportedInterfaceOrientations;
#endif
@end

@interface UIWindow : UIResponder
@property (nonatomic, assign) UIWindowScene *windowScene;
@property (nonatomic, assign) UIViewController *rootViewController;
- (void)makeKeyAndVisible;
@end

@protocol UIApplicationDelegate
@optional
#if !TARGET_OS_TV
- (UIInterfaceOrientationMask)application:(UIApplication *)application
    supportedInterfaceOrientationsForWindow:(UIWindow *)window;
#endif
@end

@protocol UISceneDelegate
@end

@protocol UIWindowSceneDelegate <UISceneDelegate>
@optional
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options;
@end

@interface UIApplication : UIResponder
@property (class, nonatomic, readonly) UIApplication *sharedApplication;
@property (nonatomic, assign) id<UIApplicationDelegate> delegate;
@property (nonatomic, readonly) NSSet *connectedScenes;
@end
