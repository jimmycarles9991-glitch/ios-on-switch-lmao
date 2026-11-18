#import <UIKit/UIKit.h>

#define WHITE_BLUR_TAG ((long)0x70757265)

@interface UINavigationBar (SwitchAppearance)
- (void)switchSetShadowHidden:(BOOL)hidden NS_SWIFT_NAME(switchSetShadowHidden(_:));
@end

@interface UIToolbar (SwitchAppearance)
- (void)switchSetShadowHidden:(BOOL)hidden NS_SWIFT_NAME(switchSetShadowHidden(_:));
@end

@interface UIViewController (SwitchAppearance)
- (void)switchConfigureScrollEdgeForScrollView:(UIScrollView *)scrollView
    NS_SWIFT_NAME(switchConfigureScrollEdge(for:));
@end
