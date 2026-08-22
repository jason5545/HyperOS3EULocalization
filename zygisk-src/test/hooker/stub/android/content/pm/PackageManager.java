package android.content.pm;

import android.content.Intent;

/** Host-side stub: test-controlled results for the two calls the hookers use. */
public class PackageManager {
    public PackageInfo packageInfo;                 // null + packageInfoThrows → throw
    public boolean packageInfoThrows;
    public ResolveInfo resolveResult;               // may be null

    public int lastResolveFlags = -1;
    public Intent lastResolveIntent;

    public static class NameNotFoundException extends Exception {
        public NameNotFoundException(String name) { super(name); }
    }

    public PackageInfo getPackageInfo(String packageName, int flags)
            throws NameNotFoundException {
        if (packageInfoThrows || packageInfo == null) {
            throw new NameNotFoundException(packageName);
        }
        return packageInfo;
    }

    public ResolveInfo resolveService(Intent intent, int flags) {
        lastResolveIntent = intent;
        lastResolveFlags = flags;
        return resolveResult;
    }
}
