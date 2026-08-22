package android.content;

import android.content.pm.PackageManager;

/** Host-side stub: settable package manager / classloader / package name. */
public class Context {
    private final PackageManager pm;
    private final ClassLoader loader;
    private final String packageName;

    public Context(PackageManager pm, ClassLoader loader, String packageName) {
        this.pm = pm;
        this.loader = loader;
        this.packageName = packageName;
    }

    public PackageManager getPackageManager() { return pm; }

    public ClassLoader getClassLoader() { return loader; }

    public String getPackageName() { return packageName; }
}
