import android.content.Context;
import android.content.Intent;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.content.pm.ServiceInfo;
import android.os.Bundle;

import com.google.android.libraries.gsa.launcherclient.LauncherClient;
import com.miui.home.launcher.Application;
import com.miui.home.launcher.BaseLauncher;
import com.miui.home.launcher.DeviceConfig;
import com.miui.home.launcher.LauncherAssistantCompat;
import com.miui.home.launcher.LauncherAssistantCompatGoogle;
import com.miui.home.launcher.LauncherAssistantCompatMIUI;
import com.miui.home.launcher.MIUIWidgetUtil;
import com.miui.home.launcher.ShortcutInfo;
import com.miui.home.library.utils.AsyncTaskExecutorHelper;
import com.miui.home.model.api.ItemInfo;

import java.lang.reflect.Field;
import java.lang.reflect.Method;

import jrc.homefeed.HomeRsaHooker;
import jrc.homefeed.MinusScreenHooker;
import jrc.homefeed.WidgetPickerHooker;

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

    private static int fakeGotoPickerCalls;

    public static void fakeGotoPicker(BaseLauncher launcher, ItemInfo itemInfo) {
        fakeGotoPickerCalls++;
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
        pm.archiveInfo = info;
        Context ctx = new Context(pm, null, "com.miui.home");
        ctx.appInfo = new ApplicationInfo();
        ctx.appInfo.sourceDir = "/product/priv-app/MiuiHome/MiuiHome.apk";
        check(HomeRsaHooker.shouldInstall(ctx),
                "pinned CN build 750062529 must install");

        info.versionCode = 601062515L;
        check(!HomeRsaHooker.shouldInstall(ctx),
                "EU build must NOT install");

        // 2026-08-26 ReSukiSU 案例：PM 登錄是 EU 2545（downgrade-keep），
        // 但執行中的 APK 是 CN 2529——必須對程式碼本體求值、放行 hook。
        PackageInfo registry = new PackageInfo();
        registry.versionCode = 750062545L;
        pm.packageInfo = registry;
        info.versionCode = 750062529L;
        check(HomeRsaHooker.shouldInstall(ctx),
                "stale PM registry must not shadow the running CN apk");

        pm.archiveInfo = null;  // archive 解析失敗（real getPackageArchiveInfo 回 null）
        check(!HomeRsaHooker.shouldInstall(ctx),
                "unparseable apk must not install");

        ctx.appInfo = null;
        check(!HomeRsaHooker.shouldInstall(ctx),
                "missing ApplicationInfo must not install");
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

    // ---- widget picker hooker cases ------------------------------------------

    private static final String PA_PKG = "com.miui.personalassistant";
    private static final String PICKER_HOME =
            PA_PKG + ".picker.business.home.pages.PickerHomeActivity";
    private static final String PICKER_DETAIL =
            PA_PKG + ".picker.business.detail.PickerDetailActivity";

    private static WidgetPickerHooker newPickerHooker() throws Exception {
        WidgetPickerHooker hooker = new WidgetPickerHooker();
        hooker.backup = backupOf("fakeGotoPicker", BaseLauncher.class, ItemInfo.class);
        fakeGotoPickerCalls = 0;
        MIUIWidgetUtil.support = false;
        DeviceConfig.p19LowMem = false;
        AsyncTaskExecutorHelper.bus.posts = 0;
        return hooker;
    }

    private static Context recordingContext() {
        Context ctx = new Context(new PackageManager(),
                HookerTestMain.class.getClassLoader(), "com.miui.home");
        Application.setInstance(new Application(ctx));
        return ctx;
    }

    private static void testWidgetPickerRerouteHome() throws Throwable {
        caseName = "widget picker reroute to picker home";
        WidgetPickerHooker hooker = newPickerHooker();
        Context ctx = recordingContext();
        BaseLauncher launcher = new BaseLauncher();

        hooker.callback(new Object[]{launcher, null});

        check(fakeGotoPickerCalls == 0, "original must NOT run when PA picker opens");
        check(ctx.lastStarted != null, "PA picker activity must be started");
        check(ctx.lastStarted != null && ctx.lastStarted.component != null
                        && PA_PKG.equals(ctx.lastStarted.component.getPackageName())
                        && PICKER_HOME.equals(ctx.lastStarted.component.getClassName()),
                "component must be PA PickerHomeActivity, got "
                        + (ctx.lastStarted == null ? null : ctx.lastStarted.component));
        if (ctx.lastStarted != null) {
            check(Integer.valueOf(2).equals(ctx.lastStarted.extrasInt.get("openSource")),
                    "openSource must be 2, got " + ctx.lastStarted.extrasInt);
            check(Integer.valueOf(10).equals(ctx.lastStarted.extrasInt.get("picker_tip_source")),
                    "picker_tip_source must be 10, got " + ctx.lastStarted.extrasInt);
            check((ctx.lastStarted.flags & 0x10000000) != 0
                            && (ctx.lastStarted.flags & 0x8000) != 0,
                    "original intent flags must be kept, got " + ctx.lastStarted.flags);
            check(ctx.lastStarted.data == null, "home picker must not carry data uri");
        }
        check(launcher.closeFolderCalls == 1 && launcher.lastCloseFolderArg,
                "closeFolder(!isCloseAnimator) must run once like the original");
        check(AsyncTaskExecutorHelper.bus.posts == 1,
                "AssistantConnectMessage must be posted like the original");
    }

    private static void testWidgetPickerRerouteDetail() throws Throwable {
        caseName = "widget picker reroute to picker detail";
        WidgetPickerHooker hooker = newPickerHooker();
        Context ctx = recordingContext();
        ShortcutInfo shortcut = new ShortcutInfo("com.xiaomi.test", "測試");

        hooker.callback(new Object[]{new BaseLauncher(), shortcut});

        check(fakeGotoPickerCalls == 0, "original must NOT run when PA detail opens");
        check(ctx.lastStarted != null && ctx.lastStarted.component != null
                        && PICKER_DETAIL.equals(ctx.lastStarted.component.getClassName()),
                "component must be PA PickerDetailActivity, got "
                        + (ctx.lastStarted == null ? null : ctx.lastStarted.component));
        if (ctx.lastStarted != null && ctx.lastStarted.data != null) {
            String uri = ctx.lastStarted.data.toString();
            check(uri.startsWith("widget://picker/detail?packageName=com.xiaomi.test"),
                    "detail uri must carry packageName, got " + uri);
            check(uri.contains("appName=測試") && uri.contains("picker_tip_source=3"),
                    "detail uri must carry appName and picker_tip_source=3, got " + uri);
        } else {
            check(false, "detail picker must carry widget://picker/detail uri");
        }
    }

    private static void testWidgetPickerSupportPassthrough() throws Throwable {
        caseName = "widget picker support=true passthrough";
        WidgetPickerHooker hooker = newPickerHooker();
        MIUIWidgetUtil.support = true;  // 原生路徑可走通：不得介入
        Context ctx = recordingContext();

        hooker.callback(new Object[]{new BaseLauncher(), null});

        check(fakeGotoPickerCalls == 1, "original must run when stock logic works");
        check(ctx.lastStarted == null, "PA picker must NOT be started by the hook");
    }

    private static void testWidgetPickerP19Passthrough() throws Throwable {
        caseName = "widget picker P19 low-mem passthrough";
        WidgetPickerHooker hooker = newPickerHooker();
        DeviceConfig.p19LowMem = true;
        Context ctx = recordingContext();

        hooker.callback(new Object[]{new BaseLauncher(), null});

        check(fakeGotoPickerCalls == 1, "P19 device must keep the original fallback");
        check(ctx.lastStarted == null, "PA picker must NOT be started on P19");
    }

    private static void testWidgetPickerStartFailureFallback() throws Throwable {
        caseName = "widget picker start failure fallback";
        WidgetPickerHooker hooker = newPickerHooker();
        Context ctx = recordingContext();
        ctx.failStart = true;  // PA 不存在/被停用時 startActivity 會丟

        hooker.callback(new Object[]{new BaseLauncher(), null});

        check(fakeGotoPickerCalls == 1,
                "start failure must fall back to the original showWidgetsPreviewLayout");
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
        testWidgetPickerRerouteHome();
        testWidgetPickerRerouteDetail();
        testWidgetPickerSupportPassthrough();
        testWidgetPickerP19Passthrough();
        testWidgetPickerStartFailureFallback();

        if (failures == 0) {
            System.out.println("ALL HOOKER TESTS PASSED");
            return;
        }
        System.out.println(failures + " FAILURE(S)");
        System.exit(1);
    }
}
