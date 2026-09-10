#include "platform/Paths.h"
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
#include <filesystem>
#include <vector>

namespace platform
{
std::string ExecutableDir()
{
    @autoreleasepool
        {
        // Inside an .app the assets live where a bundle keeps them, which is
        // not the directory holding the executable. Built as a plain
        // executable, mainBundle reports that directory and the two agree.
        NSBundle* bundle = NSBundle.mainBundle;
        if ([bundle.bundlePath hasSuffix:@".app"] && bundle.resourcePath)
            return bundle.resourcePath.UTF8String;
        }
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size ? size : 1);
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) return ".";
    std::error_code ec;
    // Resolve so a symlinked launch still finds the staged assets folder.
    const auto path = std::filesystem::weakly_canonical(std::filesystem::path(buffer.data()), ec);
    return (ec ? std::filesystem::path(buffer.data()) : path).parent_path().string();
}
std::string UserConfigDir()
{
    @autoreleasepool
        {
        NSArray<NSString*>* dirs =
            NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES);
        if (dirs.count > 0) return dirs.firstObject.UTF8String;
        return (NSHomeDirectory().UTF8String + std::string("/Library/Application Support"));
        }
}
}
