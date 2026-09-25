// See CoreFoundation/CoreFoundation.h.
#pragma once

typedef double CGFloat;
struct CGPoint { CGFloat x; CGFloat y; };
typedef struct CGPoint CGPoint;
struct CGSize { CGFloat width; CGFloat height; };
typedef struct CGSize CGSize;
struct CGRect { CGPoint origin; CGSize size; };
typedef struct CGRect CGRect;
struct CGAffineTransform { CGFloat a, b, c, d; CGFloat tx, ty; };
typedef struct CGAffineTransform CGAffineTransform;
typedef struct CGColor *CGColorRef;

CGPoint CGPointMake(CGFloat x, CGFloat y);
CGRect CGRectMake(CGFloat x, CGFloat y, CGFloat width, CGFloat height);
extern const CGAffineTransform CGAffineTransformIdentity;
CGAffineTransform CGAffineTransformMakeRotation(CGFloat angle);
