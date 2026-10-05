// macOS tray (NSStatusItem) + window helpers (NSApp).
// Compiled only on macOS through macos/Classes/system_integrator_tray.m.
// All AppKit work is dispatched to the main queue, so these functions can be
// called from any Dart thread.
#import <Cocoa/Cocoa.h>

#include "system_integrator.h"

@interface SITrayTarget : NSObject
- (void)statusItemClicked:(id)sender;
- (void)menuItemClicked:(NSMenuItem*)sender;
@end

static NSStatusItem* g_item = nil;
static SITrayTarget* g_target = nil;
static si_tray_callback g_cb = NULL;
static NSArray<NSString*>* g_labels = nil;
static NSArray<NSNumber*>* g_flags = nil;
static NSString* g_tooltip = @"";
static NSString* g_progress = @"";

static void si_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_async(dispatch_get_main_queue(), block);
  }
}

static NSString* si_str(const char* s) {
  return s ? [NSString stringWithUTF8String:s] : nil;
}

static void si_apply_text(void) {
  NSStatusBarButton* button = g_item.button;
  if (button == nil) return;
  // NSStatusItem has no progress bar: emulate with title text.
  button.title = g_progress.length ? [@" " stringByAppendingString:g_progress] : @"";
  button.toolTip = g_progress.length
                       ? [NSString stringWithFormat:@"%@ — %@", g_tooltip, g_progress]
                       : g_tooltip;
}

static void si_show_menu(void) {
  if (g_item == nil || g_labels.count == 0) return;
  NSMenu* menu = [[NSMenu alloc] init];
  menu.autoenablesItems = NO;
  for (NSUInteger i = 0; i < g_labels.count; i++) {
    int32_t f = g_flags[i].intValue;
    if (f & SI_MENU_SEPARATOR) {
      [menu addItem:[NSMenuItem separatorItem]];
      continue;
    }
    NSMenuItem* mi = [[NSMenuItem alloc] initWithTitle:g_labels[i]
                                                action:@selector(menuItemClicked:)
                                         keyEquivalent:@""];
    mi.target = g_target;
    mi.tag = (NSInteger)i;
    mi.enabled = (f & SI_MENU_ENABLED) != 0;
    mi.state = (f & SI_MENU_CHECKED) ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:mi];
  }
  g_item.menu = menu;
  [g_item.button performClick:nil];
  g_item.menu = nil;  // keep left click free for "restore"
}

@implementation SITrayTarget
- (void)statusItemClicked:(id)sender {
  NSEvent* e = [NSApp currentEvent];
  if (e.type == NSEventTypeRightMouseUp ||
      (e.modifierFlags & NSEventModifierFlagControl)) {
    si_show_menu();
  } else if (g_cb != NULL) {
    g_cb(SI_TRAY_EVENT_CLICK, -1);
  }
}
- (void)menuItemClicked:(NSMenuItem*)sender {
  if (g_cb != NULL) g_cb(SI_TRAY_EVENT_MENU, (int32_t)sender.tag);
}
@end

FFI_PLUGIN_EXPORT int32_t si_tray_init(si_tray_callback callback) {
  g_cb = callback;
  si_on_main(^{
    if (g_target == nil) g_target = [[SITrayTarget alloc] init];
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_tray_show(const char* icon_path, const char* tooltip) {
  NSString* path = si_str(icon_path);
  NSString* tip = si_str(tooltip);
  si_on_main(^{
    if (g_target == nil) g_target = [[SITrayTarget alloc] init];
    if (g_item == nil) {
      g_item = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    }
    NSStatusBarButton* button = g_item.button;
    NSImage* image = path.length ? [[NSImage alloc] initWithContentsOfFile:path] : nil;
    if (image != nil) {
      image.size = NSMakeSize(18, 18);
      image.template = [path.lowercaseString containsString:@"template"];
      button.image = image;
    } else if (button.image == nil) {
      NSImage* app = [[NSApp applicationIconImage] copy];
      app.size = NSMakeSize(18, 18);
      button.image = app;
    }
    button.imagePosition = NSImageLeft;
    button.target = g_target;
    button.action = @selector(statusItemClicked:);
    [button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    if (tip != nil) g_tooltip = tip;
    g_item.visible = YES;
    si_apply_text();
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_tray_hide(void) {
  si_on_main(^{
    if (g_item != nil) [[NSStatusBar systemStatusBar] removeStatusItem:g_item];
    g_item = nil;
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_tray_set_tooltip(const char* tooltip) {
  NSString* tip = si_str(tooltip) ?: @"";
  si_on_main(^{
    g_tooltip = tip;
    si_apply_text();
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_tray_set_progress(int32_t value, const char* label) {
  NSString* l = value == SI_PROGRESS_NONE ? @"" : (si_str(label) ?: @"");
  si_on_main(^{
    g_progress = l;
    si_apply_text();
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_tray_set_menu(const char** labels, const int32_t* flags,
                                           int32_t count) {
  NSMutableArray<NSString*>* ls = [NSMutableArray array];
  NSMutableArray<NSNumber*>* fs = [NSMutableArray array];
  for (int32_t i = 0; i < count; i++) {
    [ls addObject:si_str(labels[i]) ?: @""];
    [fs addObject:@(flags[i])];
  }
  si_on_main(^{
    g_labels = ls;
    g_flags = fs;
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT void si_tray_dispose(void) {
  g_cb = NULL;
  si_tray_hide();
}

FFI_PLUGIN_EXPORT int32_t si_window_hide(void) {
  si_on_main(^{
    for (NSWindow* w in [NSApp windows]) {
      if (w.canBecomeMainWindow) [w orderOut:nil];
    }
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_window_restore(void) {
  si_on_main(^{
    [NSApp activateIgnoringOtherApps:YES];
    for (NSWindow* w in [NSApp windows]) {
      if (!w.canBecomeMainWindow) continue;
      if (w.miniaturized) [w deminiaturize:nil];
      [w makeKeyAndOrderFront:nil];
    }
  });
  return SI_OK;
}

FFI_PLUGIN_EXPORT int32_t si_window_flash(void) {
  si_on_main(^{
    [NSApp requestUserAttention:NSCriticalRequest];  // bounces the Dock icon
  });
  return SI_OK;
}
