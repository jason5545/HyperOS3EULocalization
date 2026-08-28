package jrc.mmedit;

import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * Routes the CN MediaEditor's AI cloud features to the CN inference endpoint
 * on an EU ROM.
 *
 * 相簿編輯的「超高畫質」（/image_enhancer/provider）走 edge-cloud：
 * AigcCloud 以 android.os.SystemProperties.get("ro.miui.region")（反射、
 * 1-arg overload）當 key，查雲控 cloud_ai_config 的 region→URL 表。
 * EU 裝置 ro.miui.region=TW 命中 "tw" → sgp-ai-photo.engine.intl.miui.com；
 * 該節點 API 活著、建任務成功，但推論任務恆在約 1 秒內回 taskStatus=4003
 * （app 統一翻譯成「請求超時」toast，屬誤導）。2026-08-28 myron 實測：
 * 同帳號同圖改打 cn 端點 avatar-ai.sec.miui.com 即成功（79s 完成 2x 放大）。
 *
 * 在此攔截、只把 ro.miui.region 改成 "CN"，其餘 key 原樣放行。不修改全域
 * prop，其他進程完全看不見；mediaeditor 進程外的區域行為（含系統設定裡的
 * 台灣地區）不受影響。
 *
 * 不 pin MediaEditor 版本：hook 目標是 framework 的 SystemProperties.get，
 * shape 與 app 版本無關；最壞情況只是該進程的雲端功能改打 CN 端點。
 * （對照：HomeRsaHooker pin 桌面版本是因為它還要碰桌面內部類的 shape。）
 *
 * Hook target: android.os.SystemProperties.get(Ljava/lang/String;)Ljava/lang/String;
 * LSPlant 對 static 方法的 callback 沒有 this 佔位：args[0] 就是第一個參數；
 * backup 由 native 在 Hook() 成功後填入。
 */
public final class RegionHooker {
    private static final String TAG = "MMEdit-Region";
    private static final String KEY_REGION = "ro.miui.region";
    private static final String REGION_SPOOF = "CN";

    /** Filled by native code with the LSPlant backup method after Hook(). */
    public Method backup;

    /**
     * Target-shape guard, evaluated before any ART hook is attempted.
     * 只驗證 framework 方法存在；不依賴 app 內部類，不做版本綁定（見類註）。
     */
    public static boolean shouldInstall() {
        try {
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
        if (args != null && args.length == 1 && KEY_REGION.equals(args[0])) {
            return REGION_SPOOF;
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
}
