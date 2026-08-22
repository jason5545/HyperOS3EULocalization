import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;

import com.google.android.libraries.gsa.launcherclient.LauncherClient;
import com.miui.home.launcher.BaseLauncher;
import com.miui.home.launcher.LauncherAssistantCompat;
import com.miui.home.launcher.LauncherAssistantCompatGoogle;
import com.miui.home.launcher.LauncherAssistantCompatMIUI;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

import jrc.homefeed.HomeRsaHooker;
import jrc.homefeed.MinusScreenHooker;

/**
 * Host-side regression test for the MiuiHome homefeed hookers (no device, no
 * ART — Android APIs come from hand-rolled stubs under stub/, hook targets
 * from fake/ with the same shapes as the pinned CN build 750062529).
 *
 * Build & run: sh zygisk-src/test/run_hooker_test.sh
 */
public final class HookerTestMain {
    private static int failures = 0;
    private static String caseName = "";

    private static void check(boolean cond, String what) {
        if (!cond) {
            failures++;
            System.out.println("  FAIL [" + caseName + "] " + what);
        }
    }

    // ---- fake backup methods standing in for the hooked originals ----------

    public static String fakeSystemGet(String key) { return "real:" + key; }

    public static String fakeSystemGetThrows(String key) {
        throw new IllegalStateException("boom");
    }

    private static Object presetNewInstance;

    public static LauncherAssistantCompat fakeNewInstance(BaseLauncher launcher) {
        return (LauncherAssistantCompat) presetNewInstance;
    }

    private static Method backupOf(String name, Class<?>... params) throws Exception {
        return HookerTestMain.class.getMethod(name, params);
    }

    // ---- cases --------------------------------------------------------------

    private static void testRsaCallback() throws Throwable {
        caseName = "rsa callback";
        HomeRsaHooker hooker = new HomeRsaHooker();
        hooker.backup = backupOf("fakeSystemGet", String.class);

        Object spoofed = hooker.callback(new Object[]{"ro.com.miui.rsa"});
        check("tier1_5".equals(spoofed), "ro.com.miui.rsa must be spoofed to tier1_5, got " + spoofed);

        Object passed = hooker.callback(new Object[]{"ro.product.device"});
        check("real:ro.product.device".equals(passed), "other keys must pass through, got " + passed);
    }

    private static void testRsaCallbackRethrowsCause() throws Throwable {
        caseName = "rsa callback exception unwrap";
        HomeRsaHooker hooker = new HomeRsaHooker();
        hooker.backup = backupOf("fakeSystemGetThrows", String.class);
        try {
            hooker.callback(new Object[]{"ro.x"});
            check(false, "cause must be rethrown");
        } catch (IllegalStateException e) {
            check("boom".equals(e.getMessage()), "original cause, not InvocationTargetException");
        }
    }

    private static void testShouldInstall() {
        caseName = "shouldInstall version guard";
        PackageManager pm = new PackageManager();
        PackageInfo info = new PackageInfo();
        info.versionCode = 750062529L;
        pm.packageInfo = info;
        check(HomeRsaHooker.shouldInstall(new Context(pm, null, "com.miui.home")),
                "pinned CN build 750062529 must install");

        info.versionCode = 601062515L;
        check(!HomeRsaHooker.shouldInstall(new Context(pm, null, "com.miui.home")),
                "EU build must NOT install");

        pm.packageInfo = null;  // getPackageInfo throws NameNotFoundException
        check(!HomeRsaHooker.shouldInstall(new Context(pm, null, "com.miui.home")),
                "missing package must not install");
    }

    private static boolean pollUntil(java.util.concurrent.Callable<Boolean> cond,
                                     long timeoutMs) throws Exception {
        long deadline = System.currentTimeMillis() + timeoutMs;
        while (System.currentTimeMillis() < deadline) {
            if (cond.call()) return true;
            Thread.sleep(5);  // real sleep: the worker thread runs concurrently
        }
        return cond.call();
    }

    private static ResolveInfo resolveInfoWithVersion(int version) {
        Bundle meta = new Bundle();
        meta.putInt("service.api.version", version);
        ServiceInfo si = new ServiceInfo();
        si.metaData = meta;
        ResolveInfo ri = new ResolveInfo();
        ri.serviceInfo = si;
        return ri;
    }

    private static void testServiceVersionRewrite() throws Exception {
        caseName = "serviceVersion rewrite on stale resolve";
        LauncherClient.reset(1);  // stale legacy path, as seen after the boot race
        PackageManager pm = new PackageManager();
        pm.resolveResult = resolveInfoWithVersion(11);
        Context ctx = new Context(pm, HookerTestMain.class.getClassLoader(), "com.miui.home");
        HomeRsaHooker.ensureServiceApiVersion(ctx);
        boolean done = pollUntil(() -> LauncherClient.current() == 11, 10000);
        check(done, "static b must be rewritten to 11, got " + LauncherClient.current());
        check(pm.lastResolveFlags == 786560, "must resolve with EU flags 786560, got "
                + pm.lastResolveFlags);
    }

    private static void testServiceVersionHealthyUntouched() throws Exception {
        caseName = "serviceVersion healthy stays untouched";
        LauncherClient.reset(11);
        PackageManager pm = new PackageManager();  // resolveResult null: would fail if called
        Context ctx = new Context(pm, HookerTestMain.class.getClassLoader(), "com.miui.home");
        HomeRsaHooker.ensureServiceApiVersion(ctx);
        Thread.sleep(300);  // let the worker thread run its course
        check(LauncherClient.current() == 11, "healthy b must stay 11");
        check(pm.lastResolveFlags == -1, "resolveService must not run when b is healthy");
    }

    private static void testServiceVersionGiveUp() throws Exception {
        caseName = "serviceVersion unresolved gives up safely";
        LauncherClient.reset(1);
        PackageManager pm = new PackageManager();  // resolve always null
        Context ctx = new Context(pm, HookerTestMain.class.getClassLoader(), "com.miui.home");
        HomeRsaHooker.ensureServiceApiVersion(ctx);
        boolean tried = pollUntil(() -> pm.lastResolveFlags != -1, 10000);
        check(tried, "retry loop must have attempted resolves");
        check(pm.lastResolveFlags == 786560, "retries use EU flags, got " + pm.lastResolveFlags);
        check(LauncherClient.current() == 1, "b must stay untouched when GSA never resolves");
    }

    private static void testMinusScreenReroute() throws Throwable {
        caseName = "minus screen reroute";
        MinusScreenHooker hooker = new MinusScreenHooker();
        hooker.backup = backupOf("fakeNewInstance", BaseLauncher.class);
        BaseLauncher launcher = new BaseLauncher();

        // Google mode: passthrough, same instance
        presetNewInstance = newCompat(LauncherAssistantCompatGoogle.class,
                "com.google.android.googlequicksearchbox");
        Object out = hooker.callback(new Object[]{launcher});
        check(out == presetNewInstance, "google client must pass through untouched");

        // Real globalminusscreen installed case: passthrough
        presetNewInstance = newCompat(LauncherAssistantCompatMIUI.class,
                "com.mi.globalminusscreen");
        out = hooker.callback(new Object[]{launcher});
        check(out == presetNewInstance, "installed globalminusscreen must pass through");

        // Dead end (old package, not installed): reroute to personalassistant
        presetNewInstance = newCompat(LauncherAssistantCompatGoogle.class,
                "com.mi.android.globalminusscreen");
        out = hooker.callback(new Object[]{launcher});
        check(out instanceof LauncherAssistantCompatMIUI,
                "dead-end package must reroute to MIUI compat, got " + out);
        Field f = findField(out.getClass(), "mPackageName");
        f.setAccessible(true);
        check("com.miui.personalassistant".equals(f.get(out)),
                "rerouted client must bind com.miui.personalassistant, got " + f.get(out));

        // null from the original: stay null
        presetNewInstance = null;
        out = hooker.callback(new Object[]{launcher});
        check(out == null, "null result must stay null");
    }

    private static Field findField(Class<?> cls, String name) throws Exception {
        while (cls != null) {
            try {
                return cls.getDeclaredField(name);
            } catch (NoSuchFieldException e) {
                cls = cls.getSuperclass();
            }
        }
        throw new NoSuchFieldException(name);
    }

    /** Fake ctors are package-private like production; build via reflection. */
    private static LauncherAssistantCompat newCompat(Class<?> cls, String pkg)
            throws Exception {
        java.lang.reflect.Constructor<?> ctor =
                cls.getDeclaredConstructor(BaseLauncher.class, String.class);
        ctor.setAccessible(true);
        return (LauncherAssistantCompat) ctor.newInstance(new BaseLauncher(), pkg);
    }

    public static void main(String[] args) throws Throwable {
        testRsaCallback();
        testRsaCallbackRethrowsCause();
        testShouldInstall();
        testServiceVersionRewrite();
        testServiceVersionHealthyUntouched();
        testServiceVersionGiveUp();
        testMinusScreenReroute();

        if (failures == 0) {
            System.out.println("ALL HOOKER TESTS PASSED");
            return;
        }
        System.out.println(failures + " FAILURE(S)");
        System.exit(1);
    }
}
