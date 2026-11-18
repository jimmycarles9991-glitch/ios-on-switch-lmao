#import "WhiteBlur.h"
#import <objc/runtime.h>

static char switch_shadow_key;

static NSArray *switch_saved_shadows(id bar, NSArray *appearances, BOOL hidden)
{
    NSArray *saved = objc_getAssociatedObject(bar, &switch_shadow_key);
    if (!saved && hidden) {
        NSMutableArray *copies = [NSMutableArray arrayWithCapacity:appearances.count];
        for (id appearance in appearances)
            [copies addObject:appearance == NSNull.null ? appearance : [appearance copy]];
        saved = copies;
        objc_setAssociatedObject(bar, &switch_shadow_key, saved, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    return saved;
}

static id switch_shadow_appearance(UIBarAppearance *appearance, id original, BOOL hidden)
{
    UIBarAppearance *copy = [appearance copy];
    if (!hidden && original == NSNull.null) {
        original = [[[appearance class] alloc] init];
        [original configureWithDefaultBackground];
    }
    copy.shadowColor = hidden ? UIColor.clearColor : [original shadowColor];
    copy.shadowImage = hidden ? nil : [original shadowImage];
    return copy;
}

@implementation UINavigationBar (SwitchAppearance)

- (void)switchSetShadowHidden:(BOOL)hidden
{
    NSArray *saved = switch_saved_shadows(self,
                                          @[
                                              self.standardAppearance ?: NSNull.null,
                                              self.compactAppearance ?: NSNull.null,
                                              self.scrollEdgeAppearance ?: NSNull.null,
                                              self.compactScrollEdgeAppearance ?: NSNull.null,
                                          ],
                                          hidden);
    if (!saved)
        return;
    self.standardAppearance = switch_shadow_appearance(self.standardAppearance, saved[0], hidden);
    self.compactAppearance = switch_shadow_appearance(self.compactAppearance, saved[1], hidden);
    self.scrollEdgeAppearance = switch_shadow_appearance(self.scrollEdgeAppearance, saved[2], hidden);
    self.compactScrollEdgeAppearance = switch_shadow_appearance(self.compactScrollEdgeAppearance, saved[3], hidden);
    if (!hidden)
        objc_setAssociatedObject(self, &switch_shadow_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

@end

@implementation UIToolbar (SwitchAppearance)

- (void)switchSetShadowHidden:(BOOL)hidden
{
    NSArray *saved = switch_saved_shadows(self,
                                          @[
                                              self.standardAppearance ?: NSNull.null,
                                              self.compactAppearance ?: NSNull.null,
                                              self.scrollEdgeAppearance ?: NSNull.null,
                                              self.compactScrollEdgeAppearance ?: NSNull.null,
                                          ],
                                          hidden);
    if (!saved)
        return;
    self.standardAppearance = switch_shadow_appearance(self.standardAppearance, saved[0], hidden);
    self.compactAppearance = switch_shadow_appearance(self.compactAppearance, saved[1], hidden);
    self.scrollEdgeAppearance = switch_shadow_appearance(self.scrollEdgeAppearance, saved[2], hidden);
    self.compactScrollEdgeAppearance = switch_shadow_appearance(self.compactScrollEdgeAppearance, saved[3], hidden);
    if (!hidden)
        objc_setAssociatedObject(self, &switch_shadow_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}

@end

static UINavigationBarAppearance *switch_scroll_appearance(UINavigationBarAppearance *source, BOOL transparent)
{
    UINavigationBarAppearance *appearance = source ? [source copy] : [UINavigationBarAppearance new];
    if (transparent)
        [appearance configureWithTransparentBackground];
    else
        [appearance configureWithDefaultBackground];
    return appearance;
}

@implementation UIViewController (SwitchAppearance)

- (void)switchConfigureScrollEdgeForScrollView:(UIScrollView *)scrollView
{
    UINavigationBar *bar = self.navigationController.navigationBar;
    UINavigationBarAppearance *standard = switch_scroll_appearance(bar.standardAppearance, NO);
    UINavigationBarAppearance *compact = switch_scroll_appearance(bar.compactAppearance ?: bar.standardAppearance, NO);
    self.navigationItem.standardAppearance = standard;
    self.navigationItem.compactAppearance = compact;
    self.navigationItem.scrollEdgeAppearance = switch_scroll_appearance(standard, YES);
    self.navigationItem.compactScrollEdgeAppearance = switch_scroll_appearance(compact, YES);
    [self setContentScrollView:scrollView forEdge:NSDirectionalRectEdgeTop];
}

@end
