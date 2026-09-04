package android.credentials;

import java.util.Arrays;
import java.util.List;

/**
 * Hand-rolled stub of the framework CredentialManager — only the @SystemApi
 * shape jrc.settings.CredListHooker reflects on:
 * getCredentialProviderServices(int userId, int flags).
 */
public class CredentialManager {
    public boolean throwsOnCall = false;
    public int lastUserId = -1;
    public int lastFlags = -1;

    public List<String> getCredentialProviderServices(int userId, int flags) {
        if (throwsOnCall) throw new IllegalStateException("hidden-api blocked");
        lastUserId = userId;
        lastFlags = flags;
        return Arrays.asList("FULL:gms", "FULL:bitwarden", "FULL:xiaomi");
    }
}
