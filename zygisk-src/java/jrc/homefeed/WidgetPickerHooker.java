package jrc.homefeed;

import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * 把 CN 桌面「小工具」入口導回小部件中心。
 *
 * CN 7.50 桌面的 WidgetManagerUtils.gotoPicker 先看
 * MIUIWidgetUtil.isMIUIWidgetSupport()：支援就開
 * sAssistantWidget.getPickerHomeActivity()（小部件中心，第一套介面），
 * 不支援才退回桌面內建清單頁 showWidgetsPreviewLayout（AOSP 式列表）。
 *
 * 但 MIUIWidgetCompat 的 sAssistantWidget 依 IS_INTERNATIONAL_BUILD 選邊：
 * EU 裝置恆為 AssistantWidgetCompatGlobal，其 isSupportMIUIWidget 要求
 * com.mi.globalminusscreen（全球版負一屏）以系統 app 存在——EU 308 底包
 * 沒有它（负一屏走的是 Google/資訊助手），判定恆 false，「小工具」按鈕
 * 因此永遠直接掉進內建清單頁，小部件中心（實際由裝置上的
 * com.miui.personalassistant 25.31.01 提供，widget://picker/home 可正常
 * resolve）從來不被啟動。
 *
 * 這裡 hook gotoPicker：僅在原生判定為不支援、且非 P19 低記憶體機時，
 * 用 CN 版原本的 intent 格式（openSource=2、picker_tip_source、
 * widget://picker/detail URI 全照抄）改開 personalassistant 的
 * PickerHome/PickerDetail；任一步失敗都退回原方法，行為與未 hook 相同。
 * isMIUIWidgetSupport() 為 true 的環境（例如真的裝了全球版負一屏）
 * 完全原樣放行。
 *
 * Hook target（static，LSPlant callback 無 this 佔位，
 * args[0]=BaseLauncher、args[1]=ItemInfo 可為 null）:
 *   com.miui.home.launcher.common.WidgetManagerUtils.gotoPicker(
 *       Lcom/miui/home/launcher/BaseLauncher;Lcom/miui/home/model/api/ItemInfo;)V
 */
public final class WidgetPickerHooker {
    private static final String TAG = "HomeFeed-WidgetPicker";
    private static final String PA_PACKAGE = "com.miui.personalassistant";
    private static final String PICKER_HOME =
            PA_PACKAGE + ".picker.business.home.pages.PickerHomeActivity";
    private static final String PICKER_DETAIL =
            PA_PACKAGE + ".picker.business.detail.PickerDetailActivity";

    // 比照 gotoPicker 原樣旗標：0x10000000 =
    // MiuiWindowManagerUtils.WINDOW_EXTRA_FLAG_DISABLE_FADE_ROTATION_ANIMATION、
    // 0x8000 為原程式碼的常數 32768。
    private static final int FLAG_DISABLE_FADE_ROTATION = 0x10000000;
    private static final int FLAG_ORIGINAL_32768 = 0x8000;

    /** Filled by native code with the LSPlant backup method after Hook(). */
    public Method backup;

    public Object callback(Object[] args) throws Throwable {
        Object launcher = (args != null && args.length > 0) ? args[0] : null;
        Object itemInfo = (args != null && args.length > 1) ? args[1] : null;
        if (launcher != null && shouldReroute(launcher)) {
            try {
                openPaPicker(launcher, itemInfo);
                Log.i(TAG, "opened PA picker (detail=" + (itemInfo != null) + ")");
                return null;  // 目標回傳 void
            } catch (Throwable t) {
                Log.w(TAG, "PA picker open failed, run original", t);
            }
        }
        try {
            return backup.invoke(null, args);
        } catch (InvocationTargetException e) {
            Throwable cause = e.getCause();
            if (cause != null) throw cause;
            throw e;
        }
    }

    /** 只攔原生路徑走不通的情況；其餘交給原方法。 */
    private static boolean shouldReroute(Object launcher) {
        ClassLoader loader = launcher.getClass().getClassLoader();
        try {
            if (staticBoolean(loader,
                    "com.miui.home.launcher.MIUIWidgetUtil", "isMIUIWidgetSupport")) {
                return false;  // 原生支援（全球版負一屏在）：走原決策
            }
        } catch (Throwable t) {
            Log.w(TAG, "isMIUIWidgetSupport unavailable, keep original", t);
            return false;  // 類/方法形狀不符：當作版本漂移，安全放行
        }
        try {
            if (staticBoolean(loader,
                    "com.miui.home.launcher.DeviceConfig", "isP19LowMemDevice")) {
                return false;  // P19 低記憶體機：比照原碼退回內建清單頁
            }
        } catch (Throwable t) {
            Log.w(TAG, "isP19LowMemDevice unavailable, assume not P19", t);
        }
        return true;
    }

    private static void openPaPicker(Object launcher, Object itemInfo) throws Throwable {
        ClassLoader loader = launcher.getClass().getClassLoader();

        Intent intent = new Intent("android.intent.action.VIEW");
        intent.addFlags(FLAG_DISABLE_FADE_ROTATION);
        intent.addFlags(FLAG_ORIGINAL_32768);
        intent.putExtra("openSource", 2);
        if (itemInfo != null) {
            // 長按 app 圖示的「新增小工具」：進該 app 的小部件詳情頁
            intent.setComponent(new ComponentName(PA_PACKAGE, PICKER_DETAIL));
            String pkg = callStringMethod(itemInfo, "getPackageName");
            if (pkg != null) {
                Object title = callMethod(itemInfo, "getTitle");
                intent.setData(Uri.parse("widget://picker/detail?packageName=" + pkg
                        + "&appName=" + title + "&openSource=2&picker_tip_source=3"));
            }
        } else {
            intent.putExtra("picker_tip_source", 10);
            intent.setComponent(new ComponentName(PA_PACKAGE, PICKER_HOME));
        }

        tryCloseFolder(loader, launcher);
        tryPostAssistantConnect(loader);

        Object app = loader.loadClass("com.miui.home.launcher.Application")
                .getDeclaredMethod("getInstance")
                .invoke(null);
        Context context = (Context) app.getClass()
                .getMethod("getApplicationContext")
                .invoke(app);
        context.startActivity(intent);  // 失敗（ActivityNotFound 等）往外丟，由 callback 退回原方法
    }

    /** 比照原碼 closeFolder(!isCloseAnimator())；失敗不影響主流程。 */
    private static void tryCloseFolder(ClassLoader loader, Object launcher) {
        try {
            boolean closeAnimator = staticBoolean(loader,
                    "com.miui.home.isolate.AnimatorDurationScaleHelper", "isCloseAnimator");
            Method closeFolder = launcher.getClass()
                    .getMethod("closeFolder", boolean.class);
            closeFolder.invoke(launcher, !closeAnimator);
        } catch (Throwable t) {
            Log.w(TAG, "closeFolder skipped", t);
        }
    }

    /** 比照原碼通知 assistant overlay 刷新；失敗不影響主流程。 */
    private static void tryPostAssistantConnect(ClassLoader loader) {
        try {
            Object bus = loader.loadClass(
                            "com.miui.home.library.utils.AsyncTaskExecutorHelper")
                    .getDeclaredMethod("getEventBus")
                    .invoke(null);
            Object message = loader.loadClass(
                            "com.miui.home.launcher.overlay.assistant.AssistantConnectMessage")
                    .getDeclaredConstructor()
                    .newInstance();
            bus.getClass().getMethod("post", Object.class).invoke(bus, message);
        } catch (Throwable t) {
            Log.w(TAG, "AssistantConnectMessage skipped", t);
        }
    }

    private static boolean staticBoolean(ClassLoader loader, String className,
                                         String methodName) throws Throwable {
        Method method = loader.loadClass(className).getDeclaredMethod(methodName);
        method.setAccessible(true);
        Object value = method.invoke(null);
        return value instanceof Boolean && (Boolean) value;
    }

    private static String callStringMethod(Object target, String name) {
        Object value = callMethod(target, name);
        return value instanceof String ? (String) value : null;
    }

    private static Object callMethod(Object target, String name) {
        try {
            Method method = target.getClass().getMethod(name);
            return method.invoke(target);
        } catch (Throwable t) {
            return null;
        }
    }
}
