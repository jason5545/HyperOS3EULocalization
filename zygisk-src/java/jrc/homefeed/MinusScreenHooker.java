package jrc.homefeed;

import android.util.Log;

import java.lang.reflect.Constructor;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * 把 CN 桌面「資訊助手」模式導到裝置上實際存在的提供者。
 *
 * CN 7.50 桌面的 LauncherAssistantCompat.newInstance 在國際版分支裡，當
 * 資料來源不是 Google 且沒裝 com.mi.globalminusscreen 時，會 fallback 綁定
 * 舊版全球負一屏 com.mi.android.globalminusscreen——EU 308 底包根本沒有這個
 * App，右滑因此什麼都沒有。裝置上裝著的是 CN 智能助理
 * com.miui.personalassistant（/product/priv-app/PersonalAssistant，具備
 * com.miui.launcher.WINDOW_OVERLAY 的 AssistantOverlayService，桌面也已持有
 * miui.personalassistant.ACCESS_PROVIDER）。
 *
 * 這裡 hook newInstance：原決策回傳的 client 若綁的是不存在的舊版套件，
 * 就改用官方國行配對 LauncherAssistantCompatMIUI(launcher, personalassistant)。
 * Google 模式與已安裝全球版負一屏的情況完全原樣放行。
 *
 * Hook target（static，LSPlant callback 無 this 佔位，args[0] = BaseLauncher）:
 *   com.miui.home.launcher.LauncherAssistantCompat.newInstance(
 *       Lcom/miui/home/launcher/BaseLauncher;)
 *   Lcom/miui/home/launcher/LauncherAssistantCompat;
 */
public final class MinusScreenHooker {
    private static final String TAG = "HomeFeed-MinusScreen";
    private static final String DEAD_END_PACKAGE = "com.mi.android.globalminusscreen";
    private static final String PA_PACKAGE = "com.miui.personalassistant";
    private static final String COMPAT_MIUI =
            "com.miui.home.launcher.LauncherAssistantCompatMIUI";

    /** Filled by native code with the LSPlant backup method after Hook(). */
    public Method backup;

    public Object callback(Object[] args) throws Throwable {
        Object result;
        try {
            result = backup.invoke(null, args);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause != null) throw cause;
            throw e;
        }
        try {
            if (result == null || !DEAD_END_PACKAGE.equals(readPackageName(result))) {
                return result;
            }
            Class<?> launcherClass = result.getClass()
                    .getClassLoader()
                    .loadClass("com.miui.home.launcher.BaseLauncher");
            Class<?> miuiCompat = result.getClass()
                    .getClassLoader()
                    .loadClass(COMPAT_MIUI);
            Constructor<?> ctor = miuiCompat.getDeclaredConstructor(launcherClass,
                    String.class);
            ctor.setAccessible(true);
            Log.i(TAG, "reroute minus screen -> " + PA_PACKAGE);
            return ctor.newInstance(args[0], PA_PACKAGE);
        } catch (Throwable t) {
            // 任何反射失敗：回傳原決策結果，行為與未 hook 相同。
            Log.w(TAG, "reroute unavailable, keep original", t);
            return result;
        }
    }

    /** mPackageName 定義在 LauncherAssistantCompat 父類；沿繼承鏈往上找。 */
    private static String readPackageName(Object compat) {
        Class<?> cls = compat.getClass();
        while (cls != null) {
            try {
                Field field = cls.getDeclaredField("mPackageName");
                field.setAccessible(true);
                Object value = field.get(compat);
                return value instanceof String ? (String) value : null;
            } catch (NoSuchFieldException e) {
                cls = cls.getSuperclass();
            } catch (Throwable t) {
                return null;
            }
        }
        return null;
    }
}
