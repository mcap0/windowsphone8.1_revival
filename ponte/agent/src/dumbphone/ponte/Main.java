package dumbphone.ponte;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

// Shared helpers of the Ponte processes (Remoto, WnsServer), which run as the shell user through app_process:
// CLASSPATH=/data/local/tmp/ponte/agent.dex app_process / dumbphone.ponte.<Class>
public final class Main {
    static final String DIR = "/data/local/tmp/ponte";

    static void log(String s) {
        System.out.println(new java.text.SimpleDateFormat("HH:mm:ss").format(new java.util.Date()) + " " + s);
    }

    static String sh(String command) throws Exception {
        Process p = new ProcessBuilder("sh", "-c", command).redirectErrorStream(true).start();
        String out = readAll(p.getInputStream());
        p.waitFor();
        return out;
    }

    static String readAll(InputStream in) throws Exception {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) b.write(buf, 0, n);
        return b.toString("UTF-8");
    }

    static byte[] hmac(byte[] key, byte[] data) throws Exception {
        Mac m = Mac.getInstance("HmacSHA256");
        m.init(new SecretKeySpec(key, "HmacSHA256"));
        return m.doFinal(data);
    }

    static String hex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x));
        return sb.toString();
    }

    static byte[] hex(String s) {
        byte[] b = new byte[s.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return b;
    }
}
