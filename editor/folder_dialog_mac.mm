#include "folder_dialog.hpp"
#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace seed::editor {
bool folder_dialog_available() {
    return true;
}

std::optional<std::filesystem::path> choose_folder(const char* title, const std::filesystem::path& start) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = NO;
        panel.canChooseDirectories = YES;
        panel.canCreateDirectories = YES;
        panel.allowsMultipleSelection = NO;
        panel.message = [NSString stringWithUTF8String:title];
        if (!start.empty())
            panel.directoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:start.c_str()]
                                            isDirectory:YES];
        if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return std::nullopt;
        return std::filesystem::path(panel.URLs.firstObject.fileSystemRepresentation);
    }
}
std::optional<std::filesystem::path> choose_image(const char* title) {
    @autoreleasepool {
        NSOpenPanel* panel = [NSOpenPanel openPanel];
        panel.canChooseFiles = YES;
        panel.canChooseDirectories = NO;
        panel.allowsMultipleSelection = NO;
        panel.message = [NSString stringWithUTF8String:title];
        panel.allowedContentTypes = @[ UTTypePNG, UTTypeJPEG, [UTType typeWithFilenameExtension:@"tga"] ];
        if ([panel runModal] != NSModalResponseOK || panel.URLs.count == 0) return std::nullopt;
        return std::filesystem::path(panel.URLs.firstObject.fileSystemRepresentation);
    }
}
} // namespace seed::editor
