#include "platform/Shell.h"
#import <AppKit/AppKit.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>
#include <filesystem>

namespace platform
{
namespace
{
NSURL* FileURL(const std::string& path)
{
    NSString* text = [NSString stringWithUTF8String:path.c_str()];
    return text ? [NSURL fileURLWithPath:text] : nil;
}
}
bool OpenWithShell(const std::string& path, std::string& error)
{
    @autoreleasepool
        {
        NSURL* url = FileURL(path);
        if (url && [[NSWorkspace sharedWorkspace] openURL:url]) { error.clear(); return true; }
        error = "Could not open '" + path + "'.";
        return false;
        }
}
bool OpenFolderInExplorer(const std::string& folder, std::string& error)
{ return OpenWithShell(folder.empty() ? "/" : folder, error); }
bool ShowInExplorer(const std::string& path, std::string& error)
{
    @autoreleasepool
        {
        NSString* text = [NSString stringWithUTF8String:path.c_str()];
        // Rooted at the empty path so Finder reuses a window already showing
        // the parent, which is what selecting an item there normally does.
        if (text && [[NSWorkspace sharedWorkspace] selectFile:text inFileViewerRootedAtPath:@""])
            { error.clear(); return true; }
        }
    return OpenWithShell(std::filesystem::path(path).parent_path().string(), error);
}
bool OpenTerminalAt(const std::string& dir, std::string& error)
{
    @autoreleasepool
        {
        NSString* text = [NSString stringWithUTF8String:dir.c_str()];
        if (!text) { error = "Invalid folder path."; return false; }
        // Arguments go to the launcher as argv: a folder name must never be
        // read back as shell code.
        NSTask* task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/open"];
        task.arguments = @[@"-a", @"Terminal", @"--", text];
        NSError* failure = nil;
        if ([task launchAndReturnError:&failure]) { error.clear(); return true; }
        error = failure ? failure.localizedDescription.UTF8String : "Could not open Terminal.";
        return false;
        }
}
bool ShellThumbnail(const std::string& path, int maxDim, int& width, int& height,
                    std::vector<uint8_t>& rgba, std::string& error)
{
    @autoreleasepool
        {
        NSURL* url = FileURL(path);
        if (!url) { error = "Invalid path."; return false; }
        // Quick Look's thumbnail, the one Finder shows. The request is
        // asynchronous; this runs on a worker thread, so it just waits.
        QLThumbnailGenerationRequest* request = [[QLThumbnailGenerationRequest alloc]
            initWithFileAtURL:url size:CGSizeMake(maxDim, maxDim) scale:1.0
            representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];
        __block CGImageRef image = nullptr;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [[QLThumbnailGenerator sharedGenerator] generateBestRepresentationForRequest:request
            completionHandler:^(QLThumbnailRepresentation* representation, NSError* failure) {
                (void)failure;
                if (representation && representation.CGImage) image = CGImageRetain(representation.CGImage);
                dispatch_semaphore_signal(done);
            }];
        // A thumbnailer that hangs must not hold up the rest of the queue;
        // if it answers after this, its image is simply not used.
        const bool answered = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC)) == 0;
#if !__has_feature(objc_arc)
        [request release];
        dispatch_release(done);
#endif
        if (!answered || !image)
            {
            error = "No thumbnail for '" + path + "'.";
            return false;
            }

        width = (int)CGImageGetWidth(image);
        height = (int)CGImageGetHeight(image);
        rgba.assign((size_t)width * height * 4, 0);
        CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
        CGContextRef context = CGBitmapContextCreate(rgba.data(), (size_t)width, (size_t)height, 8, (size_t)width * 4, space,
                                                     (uint32_t)kCGImageAlphaPremultipliedLast | (uint32_t)kCGBitmapByteOrder32Big);
        CGColorSpaceRelease(space);
        if (context)
            {
            CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
            CGContextRelease(context);
            }
        CGImageRelease(image);
        if (!context || width <= 0 || height <= 0)
            {
            error = "Could not read the thumbnail of '" + path + "'.";
            return false;
            }
        error.clear();
        return true;
        }
}
}
