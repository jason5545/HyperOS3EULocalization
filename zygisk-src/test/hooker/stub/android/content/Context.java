package android.content;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;

/** Host-side stub: settable package manager / classloader / package name. */
public class Context {
    private final PackageManager pm;
    private final ClassLoader loader;
    private final String packageName;

    public Intent lastStarted;      // set by startActivity
    public boolean failStart;       // startActivity throws when true
    public ApplicationInfo appInfo; // getApplicationInfo; may be null

    public Context(PackageManager pm, ClassLoader loader, String packageName) {
        this.pm = pm;
        this.loader = loader;
        this.packageName = packageName;
    }

    public PackageManager getPackageManager() { return pm; }

    public ClassLoader getClassLoader() { return loader; }

    public String getPackageName() { return packageName; }

    public ApplicationInfo getApplicationInfo() { return appInfo; }

    public void startActivity(Intent intent) {
        if (failStart) throw new RuntimeException("activity not found: " + intent);
        lastStarted = intent;
    }
}
