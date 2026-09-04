package com.android.settings.applications.credentials;

import android.credentials.CredentialManager;

import java.util.Collections;
import java.util.List;

/**
 * Fake of the Settings build's DefaultCombinedPreferenceController — same
 * shape as the pinned target: static getCombinedProviderInfos(
 * CredentialManager, int). Its body stands in for the CN-filtered original.
 */
public class DefaultCombinedPreferenceController {
    public static List<String> getCombinedProviderInfos(CredentialManager cm, int userId) {
        return Collections.singletonList("FILTERED:xiaomi-only");
    }
}
