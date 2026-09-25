// See CoreFoundation/CoreFoundation.h.
#pragma once

#include <CoreGraphics/CoreGraphics.h>
#include <UIKit/UIKit.h>

typedef NSString *CALayerContentsFilter;
extern CALayerContentsFilter const kCAFilterLinear;
extern CALayerContentsFilter const kCAFilterNearest;

@interface CALayer : NSObject
- (instancetype)init;
@property CGRect bounds;
@property CGPoint position;
@property (getter=isHidden) BOOL hidden;
@property (getter=isOpaque) BOOL opaque;
@property CGColorRef backgroundColor;
@property (copy) CALayerContentsFilter magnificationFilter;
@property (copy) CALayerContentsFilter minificationFilter;
@property (readonly) CALayer *superlayer;
- (CGAffineTransform)affineTransform;
- (void)setAffineTransform:(CGAffineTransform)m;
- (void)addSublayer:(CALayer *)layer;
- (void)removeFromSuperlayer;
@end

@interface CATransaction : NSObject
+ (void)begin;
+ (void)commit;
+ (void)setDisableActions:(BOOL)flag;
@end
