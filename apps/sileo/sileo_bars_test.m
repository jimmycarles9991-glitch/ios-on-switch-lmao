#import <Foundation/Foundation.h>

@interface UIColor : NSObject
@property (class, readonly) UIColor *clearColor;
@end
@implementation UIColor

+ (UIColor *)clearColor
{
    static UIColor *color;
    if (!color)
        color = [UIColor new];
    return color;
}

@end

@interface UIBarAppearance : NSObject <NSCopying>
@property (strong) UIColor *shadowColor;
@property (strong) NSObject *shadowImage;
@property (strong) UIColor *backgroundColor;
@property (strong) NSObject *backgroundEffect;
@property (strong) NSObject *backgroundImage;
@property NSInteger itemStyle;
@property (copy) NSString *customSetting;
- (void)configureWithDefaultBackground;
- (void)configureWithTransparentBackground;
@end
@implementation UIBarAppearance

- (id)copyWithZone:(NSZone *)zone
{
    UIBarAppearance *copy = [[[self class] allocWithZone:zone] init];
    copy.shadowColor = self.shadowColor;
    copy.shadowImage = self.shadowImage;
    copy.backgroundColor = self.backgroundColor;
    copy.backgroundEffect = self.backgroundEffect;
    copy.backgroundImage = self.backgroundImage;
    copy.itemStyle = self.itemStyle;
    copy.customSetting = self.customSetting;
    return copy;
}

- (void)configureWithDefaultBackground
{
    self.backgroundColor = [UIColor new];
    self.backgroundEffect = [NSObject new];
    self.backgroundImage = nil;
    self.shadowColor = [UIColor new];
    self.shadowImage = nil;
}

- (void)configureWithTransparentBackground
{
    self.backgroundColor = UIColor.clearColor;
    self.backgroundEffect = nil;
    self.backgroundImage = nil;
    self.shadowColor = UIColor.clearColor;
    self.shadowImage = nil;
}

@end
@interface UINavigationBarAppearance : UIBarAppearance
@end
@implementation UINavigationBarAppearance
@end
@interface UIToolbarAppearance : UIBarAppearance
@end
@implementation UIToolbarAppearance
@end

@interface UINavigationBar : NSObject
@property (strong) UINavigationBarAppearance *standardAppearance;
@property (strong) UINavigationBarAppearance *compactAppearance;
@property (strong) UINavigationBarAppearance *scrollEdgeAppearance;
@property (strong) UINavigationBarAppearance *compactScrollEdgeAppearance;
@end
@implementation UINavigationBar
@end
@interface UIToolbar : NSObject
@property (strong) UIToolbarAppearance *standardAppearance;
@property (strong) UIToolbarAppearance *compactAppearance;
@property (strong) UIToolbarAppearance *scrollEdgeAppearance;
@property (strong) UIToolbarAppearance *compactScrollEdgeAppearance;
@end
@implementation UIToolbar
@end
@interface UINavigationController : NSObject
@property (strong) UINavigationBar *navigationBar;
@end
@implementation UINavigationController
@end
@interface UINavigationItem : NSObject
@property (strong) UINavigationBarAppearance *standardAppearance;
@property (strong) UINavigationBarAppearance *compactAppearance;
@property (strong) UINavigationBarAppearance *scrollEdgeAppearance;
@property (strong) UINavigationBarAppearance *compactScrollEdgeAppearance;
@end
@implementation UINavigationItem
@end
@interface UIScrollView : NSObject
@end
@implementation UIScrollView
@end
static const NSInteger NSDirectionalRectEdgeTop = 1;
@interface UIViewController : NSObject
@property (strong) UINavigationController *navigationController;
@property (strong) UINavigationItem *navigationItem;
@property (strong) UIScrollView *observedScrollView;
@property NSInteger observedEdge;
- (void)setContentScrollView:(UIScrollView *)scrollView forEdge:(NSInteger)edge;
@end
@implementation UIViewController

- (instancetype)init
{
    if ((self = [super init]))
        _navigationItem = [UINavigationItem new];
    return self;
}

- (void)setContentScrollView:(UIScrollView *)scrollView forEdge:(NSInteger)edge
{
    self.observedScrollView = scrollView;
    self.observedEdge = edge;
}

@end

// SWITCH_BAR_HEADER
// SWITCH_BAR_IMPLEMENTATION

static id switch_test_appearance(Class type, NSInteger marker)
{
    UIBarAppearance *appearance = [type new];
    appearance.itemStyle = marker;
    appearance.customSetting = @"preserved";
    appearance.backgroundColor = [UIColor new];
    appearance.shadowColor = [UIColor new];
    appearance.shadowImage = [NSObject new];
    appearance.backgroundImage = [NSObject new];
    return appearance;
}

int main(void)
{
    @autoreleasepool {
        UINavigationBar *bar = [UINavigationBar new];
        bar.standardAppearance = switch_test_appearance(UINavigationBarAppearance.class, 1);
        bar.compactAppearance = switch_test_appearance(UINavigationBarAppearance.class, 2);
        bar.scrollEdgeAppearance = switch_test_appearance(UINavigationBarAppearance.class, 3);
        bar.compactScrollEdgeAppearance = switch_test_appearance(UINavigationBarAppearance.class, 4);
        NSArray *originals = @[
            bar.standardAppearance, bar.compactAppearance, bar.scrollEdgeAppearance, bar.compactScrollEdgeAppearance
        ];
        [bar switchSetShadowHidden:NO];
        NSCAssert(bar.standardAppearance == originals[0], @"showing existing shadow is unchanged");
        [bar switchSetShadowHidden:YES];
        [bar switchSetShadowHidden:YES];
        NSArray *hidden = @[
            bar.standardAppearance, bar.compactAppearance, bar.scrollEdgeAppearance, bar.compactScrollEdgeAppearance
        ];
        for (NSUInteger i = 0; i < hidden.count; ++i) {
            UIBarAppearance *copy = hidden[i], *original = originals[i];
            NSCAssert(copy != original && copy.itemStyle == (NSInteger)i + 1, @"appearance copied");
            NSCAssert(copy.shadowColor == UIColor.clearColor && !copy.shadowImage, @"all shadow kinds hidden");
            NSCAssert(copy.backgroundColor == original.backgroundColor, @"background retained");
        }
        bar.standardAppearance.customSetting = @"updated while hidden";
        [bar switchSetShadowHidden:NO];
        NSArray *shown = @[
            bar.standardAppearance, bar.compactAppearance, bar.scrollEdgeAppearance, bar.compactScrollEdgeAppearance
        ];
        for (NSUInteger i = 0; i < shown.count; ++i) {
            UIBarAppearance *copy = shown[i], *original = originals[i];
            NSCAssert(copy.shadowColor == original.shadowColor && copy.shadowImage == original.shadowImage,
                      @"exact original shadow restored");
        }
        NSCAssert([bar.standardAppearance.customSetting isEqual:@"updated while hidden"], @"current style retained");
        UIToolbar *toolbar = [UIToolbar new];
        toolbar.standardAppearance = switch_test_appearance(UIToolbarAppearance.class, 5);
        UIToolbarAppearance *old = toolbar.standardAppearance;
        [toolbar switchSetShadowHidden:YES];
        [toolbar switchSetShadowHidden:NO];
        NSCAssert(toolbar.standardAppearance.shadowColor == old.shadowColor &&
                      toolbar.standardAppearance.shadowImage == old.shadowImage && !toolbar.compactAppearance &&
                      !toolbar.scrollEdgeAppearance && !toolbar.compactScrollEdgeAppearance,
                  @"toolbar fallback preserved");

        UIViewController *detail = [UIViewController new];
        detail.navigationController = [UINavigationController new];
        detail.navigationController.navigationBar = bar;
        UINavigationBarAppearance *rootStyle = bar.standardAppearance;
        UINavigationBarAppearance *compactStyle = bar.compactAppearance;
        UIScrollView *scroll = [UIScrollView new];
        [detail switchConfigureScrollEdgeForScrollView:scroll];
        NSCAssert(detail.observedScrollView == scroll && detail.observedEdge == NSDirectionalRectEdgeTop,
                  @"native containers observe real scrolling");
        NSCAssert(detail.navigationItem.standardAppearance != rootStyle &&
                      detail.navigationItem.standardAppearance.itemStyle == 1 &&
                      detail.navigationItem.compactAppearance.itemStyle == 2,
                  @"item styles copied per controller");
        NSCAssert(detail.navigationItem.standardAppearance.backgroundEffect &&
                      detail.navigationItem.compactAppearance.backgroundEffect,
                  @"native default materials");
        NSCAssert(!detail.navigationItem.scrollEdgeAppearance.backgroundEffect &&
                      detail.navigationItem.scrollEdgeAppearance.backgroundColor == UIColor.clearColor &&
                      !detail.navigationItem.compactScrollEdgeAppearance.backgroundEffect,
                  @"native scroll edges transparent in both layouts");
        NSCAssert(bar.standardAppearance == rootStyle && bar.compactAppearance == compactStyle,
                  @"shared navigation bar state preserved for pop and other tabs");
        UIViewController *unattached = [UIViewController new];
        [unattached switchConfigureScrollEdgeForScrollView:scroll];
        NSCAssert(unattached.navigationItem.standardAppearance && unattached.navigationItem.compactAppearance &&
                      unattached.observedScrollView == scroll,
                  @"pre-container setup uses native defaults");
    }
    return 0;
}
