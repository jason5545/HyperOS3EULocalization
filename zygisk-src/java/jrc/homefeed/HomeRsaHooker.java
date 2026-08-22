package jrc.homefeed;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.ResolveInfo;
import android.net.Uri;
import android.os.Process;
import android.os.SystemClock;
import android.util.Log;

import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/**
 * Keeps the Google feed available on the bundled CN MiuiHome (7.50.06.2529,
 * versionCode 750062529) running on an EU ROM.
 *
 * CN 版桌面的 DeviceConfig.isUseGoogleMinusScreen() 要求 ro.com.miui.rsa /
 * ro.com.miui.rsa.search 命中運營商清單，或 ro.miui.customized.region 落在
 * {mx_telcel, lm_cr}；EU 裝置這些 prop 全空，因此永遠走不到
 * LauncherAssistantCompatGoogle，負一屏資料來源沒有 Google。
 *
 * CN 桌面透過 com.miui.launcher.utils.SystemProperties 以反射呼叫
 * framework 的 android.os.SystemProperties.get(String) 讀 prop；在此攔截、
 * 只把 ro.com.miui.rsa 改成 "tier1_5"（SELECT_MINUS_SCREEN_CLIENT_ID 命中，
 * CAN_SWITCH_MINUS_SCREEN=true），其餘 key 原樣放行。是否用 Google 仍由
 * settings 的 switch_personal_assistant 決定，設定頁保留可切換清單，
 * 行為比照 EU 版桌面。不修改全域 prop，其他進程（Wallet/GMS）完全看不見。
 *
 * Hook target: android.os.SystemProperties.get(Ljava/lang/String;)Ljava/lang/String;
 * LSPlant 對 static 方法的 callback 沒有 this 佔位：args[0] 就是第一個參數；
 * backup 由 native 在 Hook() 成功後填入。
 */
public final class HomeRsaHooker {
    private static final String TAG = "HomeFeed-RSA";
    private static final long EXPECTED_VERSION_CODE = 750062529L;
    private static final String KEY_RSA = "ro.com.miui.rsa";
    private static final String RSA_SPOOF = "tier1_5";

    // --- LauncherClient serviceVersion 保底 ---
    // CN 版內嵌的 launcherclient 以 resolveService(intent, 128) 讀取 GSA
    // DrawerOverlayService 的 meta-data service.api.version，且 static 欄位 b
    // 只解析一次（b<=0 才重讀）。EU 版帶 MATCH_DIRECT_BOOT_*（786560）。
    // 實測同機同 GSA：EU 得 11、CN 得 1——CN 版開機時若遇上 GSA 剛更新、
    // 元件被動態停用（cfbv 依 Acetone 資格 setComponentEnabledSetting）等
    // 暫態，會永遠卡在 legacy attach（b<3），GSA 端 service_status 恆為 0，
    // 負一屏無法 attach。這裡在類載入後用 EU 旗標重解析並直接改寫 b，
    // 附带重試以熬過開機競態。
    private static final String LAUNCHER_CLIENT =
            "com.google.android.libraries.gsa.launcherclient.LauncherClient";
    private static final String GSA_PACKAGE = "com.google.android.googlequicksearchbox";
    private static final String OVERLAY_ACTION = "com.android.launcher3.WINDOW_OVERLAY";
    private static final int RESOLVE_FLAGS_EU = 786560;  // GET_META_DATA | MATCH_DIRECT_BOOT_AWARE | MATCH_DIRECT_BOOT_UNAWARE
    private static final int MIN_WORKING_API = 3;

    /** Filled by native code with the LSPlant backup method after Hook(). */
    public Method backup;

    /**
     * Version + target guard, evaluated before any ART hook is attempted.
     * 只在模組內建的 CN 桌面版本上啟用；EU 桌面不讀這個 prop，無需 hook。
     */
    public static boolean shouldInstall(Context context) {
        try {
            PackageInfo info = context.getPackageManager()
                    .getPackageInfo("com.miui.home", 0);
            long versionCode = info.getLongVersionCode();
            if (versionCode != EXPECTED_VERSION_CODE) {
                Log.w(TAG, "skip: MiuiHome versionCode " + versionCode
                        + " != " + EXPECTED_VERSION_CODE);
                return false;
            }
            Class.forName("android.os.SystemProperties")
                    .getMethod("get", String.class);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "skip: SystemProperties.get unavailable", t);
            return false;
        }
    }

    /** LSPlant callback. Signature fixed as Object callback(Object[] args). */
    public Object callback(Object[] args) throws Throwable {
        // static target：args 就是參數本體，args[0] = key。
        if (args != null && args.length == 1 && KEY_RSA.equals(args[0])) {
            return RSA_SPOOF;
        }
        try {
            return backup.invoke(null, args);
        } catch (InvocationTargetException e) {
            // 原方法自己丟例外：原樣往外拋，語意與未 hook 相同。
            Throwable cause = e.getCause();
            if (cause != null) throw cause;
            throw e;
        }
    }

    /**
     * 由 native worker 在 hook 安裝後呼叫。背景執行緒輪詢：
     * LauncherClient 類出現後檢查 static int b，若 < MIN_WORKING_API 就用
     * EU 版旗標自行 resolveService 並改寫；失敗則重試，最多約五分鐘。
     * 任何一步失敗只記 log、安全結束。
     */
    public static void ensureServiceApiVersion(final Context context) {
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    runLoop(context);
                } catch (Throwable t) {
                    Log.e(TAG, "ensureServiceApiVersion failed", t);
                }
            }
        }, "HomeFeed-SvcVer").start();
    }

    private static void runLoop(Context context) throws Throwable {
        ClassLoader loader = context.getClassLoader();
        Class<?> client = null;
        for (int i = 0; i < 120 && client == null; i++) {
            try {
                client = loader.loadClass(LAUNCHER_CLIENT);
            } catch (ClassNotFoundException e) {
                SystemClock.sleep(1000);  // 類在 Launcher.onCreate 才載入
            }
        }
        if (client == null) {
            Log.w(TAG, "LauncherClient class never appeared, give up");
            return;
        }
        Field versionField = client.getDeclaredField("b");
        if (!Modifier.isStatic(versionField.getModifiers())
                || versionField.getType() != int.class) {
            Log.w(TAG, "field b not the expected static int, give up");
            return;
        }
        versionField.setAccessible(true);

        Intent intent = new Intent(OVERLAY_ACTION)
                .setPackage(GSA_PACKAGE)
                .setData(Uri.parse("app://" + context.getPackageName()
                                + ":" + Process.myUid())
                        .buildUpon()
                        .appendQueryParameter("v", "10")
                        .appendQueryParameter("cv", "17")
                        .build());
        PackageManager pm = context.getPackageManager();
        for (int i = 0; i < 150; i++) {
            int current = versionField.getInt(null);
            if (current >= MIN_WORKING_API) {
                Log.i(TAG, "serviceVersion ok: " + current);
                return;
            }
            ResolveInfo info = pm.resolveService(intent, RESOLVE_FLAGS_EU);
            if (info != null && info.serviceInfo != null
                    && info.serviceInfo.metaData != null) {
                int version = info.serviceInfo.metaData
                        .getInt("service.api.version", 1);
                versionField.setInt(null, version);
                Log.i(TAG, "serviceVersion rewritten: " + current + " -> " + version);
                if (version >= MIN_WORKING_API) {
                    return;
                }
            }
            SystemClock.sleep(2000);  // GSA 開機初始化完成前會持續失敗
        }
        Log.w(TAG, "serviceVersion still unresolved after retries");
    }
}
