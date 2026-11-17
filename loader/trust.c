// plugin trust check: is it quarantined (downloaded, never opened through gatekeeper),
// is the signature valid, who signed it. plugins get full access to the game so the
// console shows this next to each one. nothing here blocks a plugin, dyld already refuses
// files with a broken signature.
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>
#include "internal.h"

int plugin_trust(const char *path, char *out, size_t n) {
    int flags = 0;
    if (getxattr(path, "com.apple.quarantine", NULL, 0, 0, 0) > 0) flags |= TRUST_QUARANTINED;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
    SecStaticCodeRef code = NULL;
    char who[128] = "";
    if (url && SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code) == errSecSuccess) {
        OSStatus st = SecStaticCodeCheckValidity(code, kSecCSDefaultFlags, NULL);
        if (st == errSecCSUnsigned) flags |= TRUST_UNSIGNED;
        else if (st != errSecSuccess) flags |= TRUST_BAD_SIGNATURE;
        CFDictionaryRef info = NULL;
        if (!(flags & (TRUST_UNSIGNED | TRUST_BAD_SIGNATURE)) &&
            SecCodeCopySigningInformation(code, kSecCSSigningInformation, &info) == errSecSuccess && info) {
            CFNumberRef f = CFDictionaryGetValue(info, kSecCodeInfoFlags);
            uint32_t cf = 0;
            if (f) CFNumberGetValue(f, kCFNumberSInt32Type, &cf);
            CFArrayRef certs = CFDictionaryGetValue(info, kSecCodeInfoCertificates);
            if ((cf & kSecCodeSignatureAdhoc) || !certs || !CFArrayGetCount(certs)) flags |= TRUST_ADHOC;
            else {
                SecCertificateRef leaf = (SecCertificateRef)CFArrayGetValueAtIndex(certs, 0);
                CFStringRef s = SecCertificateCopySubjectSummary(leaf);
                if (s) { CFStringGetCString(s, who, sizeof who, kCFStringEncodingUTF8); CFRelease(s); }
            }
            CFRelease(info);
        }
        CFRelease(code);
    } else flags |= TRUST_UNSIGNED;
    if (url) CFRelease(url);

    if (out && n) {
        if (flags & TRUST_BAD_SIGNATURE) snprintf(out, n, "BROKEN SIGNATURE");
        else if (flags & TRUST_UNSIGNED) snprintf(out, n, "unsigned");
        else if (flags & TRUST_ADHOC) snprintf(out, n, "ad-hoc signed");
        else snprintf(out, n, "signed: %s", who[0] ? who : "Developer ID");
        if (flags & TRUST_QUARANTINED) strncat(out, ", quarantined", n - strlen(out) - 1);
    }
    return flags;
}
