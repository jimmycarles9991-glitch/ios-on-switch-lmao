#ifndef SWITCH_INSTALLER_HANDLER_H
#define SWITCH_INSTALLER_HANDLER_H

#import <Foundation/Foundation.h>

BOOL switch_register_installer_handler(NSError **error);
id switch_installed_record(NSString *path, NSString *identifier, NSError **error);

#endif
