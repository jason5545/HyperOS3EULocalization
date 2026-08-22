package android.net;

/** Host-side stub: keeps the raw string, builder is a no-op chain. */
public class Uri {
    private final String value;

    private Uri(String value) { this.value = value; }

    public static Uri parse(String s) { return new Uri(s); }

    public Builder buildUpon() { return new Builder(this); }

    @Override
    public String toString() { return value; }

    public static final class Builder {
        private final Uri base;

        private Builder(Uri base) { this.base = base; }

        public Builder appendQueryParameter(String key, String value) { return this; }

        public Uri build() { return base; }
    }
}
