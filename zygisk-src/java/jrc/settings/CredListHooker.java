package jrc.settings;

import android.util.Log;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;

/**
 * 只讓系統設定的「密碼、密碼金鑰與自動填入」頁看到完整 credential
 * provider 清單；Settings 進程的其餘部分維持 CN flip 不變（翻該頁，不翻
 * 整個 app）。
 *
 * 根因（2026-09-04 myron；xiaomi.eu OS3.0.309 與 TW Global OS3.0.1.0 的
 * Settings.apk 比對，此處程式碼逐行相同，純執行期旗標差異）：
 * DefaultCombinedPreferenceController.getCombinedProviderInfos 在
 * miui.os.Build.IS_INTERNATIONAL_BUILD=false 時，把
 * CredentialManager.getCredentialProviderServices 的回傳過濾到只剩小米自家
 * 兩個 provider（com.miui.passwords / com.miui.contentcatcher 的
 * XiaomiCredentialProviderService）。本模組的 Taplus flip 把 Settings 進程
 * 的該旗標翻成 false，於是：
 *  - DefaultCombinedPicker（單選預設頁）只看見第三方 app 的 autofill 面，
 *    setDefaultKey 走 CN 保險分支，credential_service 被凍結或強制小米預設
 *    ——頁面表現形同「只管自動填入」；
 *  - CredentialManagerPreferenceController（額外提供者開關區）因
 *    hasNonPrimaryServices()=false 整區塊隱藏。
 * 在此把本方法替換成它自己的 INTL 分支（原樣回傳完整清單），頁面其餘邏輯
 * （CN 保險分支、確認對話框、autofill 寫入）一律不動。
 *
 * 不 pin Settings 版本：hook 是純函式替換，shape 不符時 shouldInstall 回
 * false 安全停用；最壞情況只是退回未 hook 的 CN 過濾行為。（對照：
 * HomeRsaHooker 會改寫 app 內部 static 欄位，才需要版本綁定。）
 *
 * Hook target:
 * com.android.settings.applications.credentials.DefaultCombinedPreferenceController
 * .getCombinedProviderInfos(Landroid/credentials/CredentialManager;I)
 * Ljava/util/List;
 * LSPlant 對 static 方法的 callback 沒有 this 佔位：args[0]/args[1] 就是
 * 兩個參數本體；backup 由 native 在 Hook() 成功後填入。
 */
public final class CredListHooker {
    private static final String TAG = "Settings-CredList";
    private static final String TARGET_CLASS =
            "com.android.settings.applications.credentials.DefaultCombinedPreferenceController";
    private static final String TARGET_METHOD = "getCombinedProviderInfos";
    private static final String FULL_LIST_METHOD = "getCredentialProviderServices";
    // 與該類 INTL 分支相同的呼叫：getCredentialProviderServices(userId, 2)。
    private static final int FULL_LIST_FLAGS = 2;

    /** Filled by native code with the LSPlant backup method after Hook(). */
    public Method backup;

    /**
     * Target-shape guard, evaluated before any ART hook is attempted.
     * 驗證目標方法與反射用的 @SystemApi full-list 方法兩者都在；缺一即
     * 安全停用（不裝 hook，頁面維持原 CN 行為）。
     */
    public static boolean shouldInstall() {
        try {
            ClassLoader cl = CredListHooker.class.getClassLoader();
            Class<?> credentialManager = cl.loadClass("android.credentials.CredentialManager");
            cl.loadClass(TARGET_CLASS)
                    .getDeclaredMethod(TARGET_METHOD, credentialManager, int.class);
            // android.jar 不含此 @SystemApi 方法，只能執行期反射取得。
            credentialManager.getMethod(FULL_LIST_METHOD, int.class, int.class);
            return true;
        } catch (Throwable t) {
            Log.e(TAG, "skip: target shape unavailable", t);
            return false;
        }
    }

    /** LSPlant callback. Signature fixed as Object callback(Object[] args). */
    public Object callback(Object[] args) throws Throwable {
        // static target：args[0] = CredentialManager，args[1] = userId。
        if (args != null && args.length == 2) {
            try {
                Method fullList = args[0].getClass()
                        .getMethod(FULL_LIST_METHOD, int.class, int.class);
                return fullList.invoke(args[0], args[1], FULL_LIST_FLAGS);
            } catch (Throwable t) {
                // @SystemApi 反射被擋或方法行為改變：退回原方法（CN 過濾），
                // 絕不讓 Settings 因 hook 崩潰。
                Log.e(TAG, "full-list call failed, using original", t);
            }
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
