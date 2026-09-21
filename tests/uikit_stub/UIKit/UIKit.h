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

@interface UIWindowScene : UIScene
@end

@interface UIWindow : UIResponder
@property (nonatomic, assign) UIWindowScene *windowScene;
- (void)makeKeyAndVisible;
@end

@protocol UIApplicationDelegate
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
