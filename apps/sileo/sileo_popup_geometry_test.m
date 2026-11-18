#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>

typedef struct {
    CGFloat top, left, bottom, right;
} UIEdgeInsets;

static UIEdgeInsets _LNPopupSafeAreas(id object)
{
    return (UIEdgeInsets){0};
}

@interface UIView : NSObject
@property CGRect bounds;
@property CGRect frame;
@property UIEdgeInsets safeAreaInsets;
@end
@implementation UIView
@end

@interface PopupBar : UIView
@property BOOL isInlineWithTabBar;
@end
@implementation PopupBar
@end

@interface Container : NSObject
@property (strong) UIView *view;
@property (strong) UIView *bottomDockingViewForPopupBar;
@property (strong) id presentingViewController;
@property (strong) NSObject *nonMemoryLeakingPresentationController;
@property CGRect defaultFrameForBottomDockingView;
@property UIEdgeInsets insetsForBottomDockingView;
- (CGRect)defaultFrameForBottomDockingView_internal;
- (CGRect)defaultFrameForBottomDockingView_internalOrDeveloper;
@end

@interface Probe : NSObject
@property (strong) Container *containerController;
@property (strong) PopupBar *popupBar;
- (CGRect)_frameForOpenPopupBar;
- (CGRect)_frameForClosedPopupBar;
@end

@implementation Container

- (void)containerMethods
{}

@end

@implementation Probe

- (void)popupMethods
{}

@end

int main(void)
{
    @autoreleasepool {
        Container *container = [Container new];
        container.view = [UIView new];
        container.view.bounds = CGRectMake(0, 0, 1024, 576);
        container.view.safeAreaInsets = (UIEdgeInsets){.bottom = 24};
        container.defaultFrameForBottomDockingView = CGRectMake(9, 9, 9, 9);
        Probe *probe = [Probe new];
        probe.containerController = container;
        probe.popupBar = [PopupBar new];
        probe.popupBar.frame = CGRectMake(0, 0, 1024, 64);
        NSCAssert(CGRectEqualToRect([probe _frameForClosedPopupBar], CGRectMake(0, 488, 1024, 64)),
                  @"native bottom safe area");
        NSCAssert(CGRectEqualToRect([probe _frameForOpenPopupBar], CGRectMake(0, -64, 1024, 64)),
                  @"full width open queue");
        container.view.bounds = CGRectMake(0, 0, 640, 800);
        container.view.safeAreaInsets = (UIEdgeInsets){.bottom = 0};
        NSCAssert(CGRectEqualToRect([probe _frameForClosedPopupBar], CGRectMake(0, 736, 640, 64)),
                  @"native bounds resize");
        container.view.bounds = CGRectMake(0, 0, 1024, 576);
        container.bottomDockingViewForPopupBar = [UIView new];
        container.defaultFrameForBottomDockingView = CGRectMake(0, 527, 704, 49);
        container.insetsForBottomDockingView = (UIEdgeInsets){.top = 49, .left = 704};
        probe.popupBar.isInlineWithTabBar = YES;
        NSCAssert(CGRectEqualToRect([probe _frameForClosedPopupBar], CGRectMake(704, 512, 320, 64)),
                  @"legacy inline queue");
        NSCAssert(CGRectEqualToRect([probe _frameForOpenPopupBar], CGRectMake(704, -64, 320, 64)),
                  @"legacy inline content width");
    }
    return 0;
}
