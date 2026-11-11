package dumbphone.pixelfoto;

import android.content.Context;
import android.media.MediaScannerConnection;
import android.os.Environment;
import android.provider.MediaStore;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.text.ParseException;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.TimeZone;
import java.util.concurrent.ConcurrentHashMap;

/**
 * Uploads in progress (files/inbox/&lt;sha256&gt;.part + .json) and the registry of photos received
 * (files/registry.tsv: sha256, Lumia name, DCIM name, received ms, state). Only files in the registry are ever deleted.
 */
final class Store {
    static final long KEEP_MS = 7L * 24 * 3600 * 1000;
    static final long STALE_PART_MS = 3L * 24 * 3600 * 1000;
    static final String DCIM_STATE = "dcim", DELETED = "cancellata";

    static final class Rec {
        String sha, src, file, state;
        long received;
        JSONObject json() throws Exception {
            return new JSONObject().put("sha256", sha).put("name", src).put("file", file).put("received", received)
                    .put("state", state);
        }
    }

    final Context ctx;
    final File inbox, regFile, camera;
    final byte[] template;
    private final LinkedHashMap<String, Rec> recs = new LinkedHashMap<>();
    private final ConcurrentHashMap<String, Object> locks = new ConcurrentHashMap<>();
    private final SecureRandom rnd = new SecureRandom();

    Store(Context ctx) throws IOException {
        this.ctx = ctx;
        inbox = new File(ctx.getFilesDir(), "inbox");
        inbox.mkdirs();
        regFile = new File(ctx.getFilesDir(), "registry.tsv");
        camera = new File(Environment.getExternalStorageDirectory(), "DCIM/Camera");
        try (InputStream in = ctx.getAssets().open("pixel-exif.tiff")) { template = Exif.readAll(in); }
        load();
    }

    Object lock(String id) { return locks.computeIfAbsent(id, k -> new Object()); }

    static boolean validId(String id) { return id != null && id.matches("[0-9a-f]{64}"); }

    File part(String id) { return new File(inbox, id + ".part"); }
    File meta(String id) { return new File(inbox, id + ".json"); }

    synchronized Rec find(String sha) { return recs.get(sha); }

    synchronized int count() { return recs.size(); }

    synchronized JSONArray list(int max) throws Exception {
        List<Rec> all = new ArrayList<>(recs.values());
        JSONArray a = new JSONArray();
        for (int i = all.size() - 1; i >= 0 && a.length() < max; i--) a.put(all.get(i).json());
        return a;
    }

    // ---- registry ----

    private void load() throws IOException {
        if (!regFile.exists()) return;
        String s;
        try (InputStream in = new FileInputStream(regFile)) { s = new String(Exif.readAll(in), StandardCharsets.UTF_8); }
        for (String line : s.split("\n")) {
            String[] f = line.split("\t");
            if (f.length < 5) continue;
            Rec r = new Rec();
            r.sha = f[0]; r.src = f[1]; r.file = f[2]; r.received = Long.parseLong(f[3]); r.state = f[4];
            recs.put(r.sha, r);
        }
    }

    private void save() throws IOException {
        StringBuilder sb = new StringBuilder();
        for (Rec r : recs.values())
            sb.append(r.sha).append('\t').append(clean(r.src)).append('\t').append(r.file).append('\t')
                    .append(r.received).append('\t').append(r.state).append('\n');
        File tmp = new File(regFile.getPath() + ".tmp");
        try (FileOutputStream o = new FileOutputStream(tmp)) {
            o.write(sb.toString().getBytes(StandardCharsets.UTF_8));
            o.getFD().sync();
        }
        if (!tmp.renameTo(regFile)) throw new IOException("registro non salvato");
    }

    private static String clean(String s) { return s == null ? "" : s.replace('\t', ' ').replace('\n', ' '); }

    // ---- upload ----

    /** Starts or resumes an upload. */
    JSONObject begin(JSONObject req) throws Exception {
        String sha = req.getString("sha256").toLowerCase(Locale.ROOT);
        if (!validId(sha)) throw new BadRequest("sha256 non valido");
        long size = req.getLong("size");
        if (size <= 0 || size > 200L << 20) throw new BadRequest("dimensione non valida");
        Rec done = find(sha);
        if (done != null) return new JSONObject().put("id", sha).put("status", "done").put("file", done.file)
                .put("state", done.state);
        synchronized (lock(sha)) {
            File m = meta(sha), p = part(sha);
            JSONObject info = new JSONObject().put("name", req.optString("name", "")).put("size", size)
                    .put("taken", req.optLong("taken", 0));
            try (FileOutputStream o = new FileOutputStream(m)) { o.write(info.toString().getBytes(StandardCharsets.UTF_8)); }
            long have = p.exists() ? p.length() : 0;
            if (have > size) { p.delete(); have = 0; }
            return new JSONObject().put("id", sha).put("status", "upload").put("offset", have);
        }
    }

    JSONObject info(String id) throws Exception {
        File m = meta(id);
        if (!m.exists()) return null;
        try (InputStream in = new FileInputStream(m)) {
            return new JSONObject(new String(Exif.readAll(in), StandardCharsets.UTF_8));
        }
    }

    JSONObject state(String id) throws Exception {
        Rec done = find(id);
        if (done != null) return new JSONObject().put("id", id).put("status", "done").put("file", done.file)
                .put("state", done.state);
        JSONObject info = info(id);
        if (info == null) return new JSONObject().put("id", id).put("status", "unknown");
        long have = part(id).exists() ? part(id).length() : 0;
        return new JSONObject().put("id", id).put("status", "upload").put("offset", have)
                .put("size", info.getLong("size"));
    }

    /** Appends `len` bytes at `offset`; the offset must be the current end of the part file. */
    long chunk(String id, long offset, InputStream body, long len) throws Exception {
        synchronized (lock(id)) {
            JSONObject info = info(id);
            if (info == null) throw new BadRequest("upload sconosciuto: rifai begin");
            File p = part(id);
            long have = p.exists() ? p.length() : 0;
            if (offset != have) throw new Conflict(have);
            if (offset + len > info.getLong("size")) throw new BadRequest("oltre la dimensione dichiarata");
            try (RandomAccessFile f = new RandomAccessFile(p, "rw")) {
                f.seek(offset);
                byte[] buf = new byte[1 << 16];
                long left = len, t0 = System.currentTimeMillis();
                try {
                    while (left > 0) {
                        int n = body.read(buf, 0, (int) Math.min(buf.length, left));
                        if (n < 0) break; // keep what arrived: the client resumes from the new end
                        f.write(buf, 0, n);
                        left -= n;
                    }
                } catch (IOException e) {
                    Log.i("chunk " + id.substring(0, 8) + "@" + offset + ": ricevuti " + (len - left) + " di " + len + " in "
                            + (System.currentTimeMillis() - t0) + " ms, poi " + e);
                    throw e;
                }
                if (len >= 100000) Log.i("chunk " + id.substring(0, 8) + "@" + offset + ": " + len + " byte in "
                        + (System.currentTimeMillis() - t0) + " ms");
                return f.length();
            }
        }
    }

    /** Checks the hash, writes the Pixel-style file into DCIM/Camera and registers it. */
    JSONObject end(String id) throws Exception {
        synchronized (lock(id)) {
            Rec done = find(id);
            if (done != null) return new JSONObject().put("id", id).put("status", "done").put("file", done.file)
                    .put("state", done.state);
            JSONObject info = info(id);
            if (info == null) throw new BadRequest("upload sconosciuto");
            File p = part(id);
            long size = info.getLong("size"), have = p.exists() ? p.length() : 0;
            if (have != size) return new JSONObject().put("id", id).put("status", "upload").put("offset", have);
            String real = sha256(p);
            if (!real.equals(id)) {
                p.delete();
                throw new BadRequest("sha256 diverso: file rovinato, rimandalo");
            }
            camera.mkdirs();
            File tmp = new File(camera, ".fotopixel-" + id.substring(0, 12) + ".tmp");
            TimeZone tz = TimeZone.getDefault();
            long takenMs = info.optLong("taken", 0);
            String fallback = exifDate(takenMs > 0 ? takenMs : System.currentTimeMillis(), tz);
            Exif.Result r = Exif.rewrite(p, tmp, template, fallback, t -> offset(t, tz),
                    String.format(Locale.ROOT, "%06d", rnd.nextInt(1000000)));
            File out;
            synchronized (this) {
                String base = "IMG_" + r.taken.substring(0, 4) + r.taken.substring(5, 7) + r.taken.substring(8, 10)
                        + "_" + r.taken.substring(11, 13) + r.taken.substring(14, 16) + r.taken.substring(17, 19);
                out = new File(camera, base + ".jpg");
                for (int i = 1; out.exists() || nameTaken(out.getName()); i++) out = new File(camera, base + "_" + i + ".jpg");
                if (!tmp.renameTo(out)) { tmp.delete(); throw new IOException("rename in DCIM non riuscito"); }
                long ms = parseExif(r.taken, tz);
                if (ms > 0) out.setLastModified(ms);
                Rec rec = new Rec();
                rec.sha = id; rec.src = info.optString("name", ""); rec.file = out.getName();
                rec.received = System.currentTimeMillis(); rec.state = DCIM_STATE;
                recs.put(id, rec);
                save();
            }
            MediaScannerConnection.scanFile(ctx, new String[]{out.getPath()}, new String[]{"image/jpeg"}, null);
            p.delete();
            meta(id).delete();
            locks.remove(id);
            Log.i("salvata " + out.getName() + " (" + info.optString("name") + ", " + r.width + "x" + r.height + ")");
            return new JSONObject().put("id", id).put("status", "saved").put("file", out.getName())
                    .put("taken", r.taken);
        }
    }

    private boolean nameTaken(String name) {
        for (Rec r : recs.values()) if (name.equals(r.file)) return true;
        return false;
    }

    // ---- cleanup ----

    /** Removes from DCIM the photos received more than 7 days ago (registry only) and stale partial uploads. */
    void cleanup() {
        long now = System.currentTimeMillis();
        List<Rec> old = new ArrayList<>();
        synchronized (this) {
            for (Rec r : recs.values()) if (DCIM_STATE.equals(r.state) && now - r.received > KEEP_MS) old.add(r);
        }
        for (Rec r : old) {
            File f = new File(camera, r.file);
            try {
                ctx.getContentResolver().delete(MediaStore.Images.Media.EXTERNAL_CONTENT_URI,
                        MediaStore.Images.Media.DATA + "=?", new String[]{f.getPath()});
            } catch (Exception e) {
                Log.i("MediaStore delete " + r.file + ": " + e);
            }
            if (f.exists() && !f.delete()) { Log.i("non cancellata: " + r.file); continue; }
            synchronized (this) {
                r.state = DELETED;
                try { save(); } catch (IOException e) { Log.i("registro: " + e); }
            }
            Log.i("cancellata dopo 7 giorni: " + r.file);
        }
        File[] parts = inbox.listFiles();
        if (parts != null) for (File f : parts) if (now - f.lastModified() > STALE_PART_MS) f.delete();
    }

    // ---- helpers ----

    static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = in.read(buf)) > 0) md.update(buf, 0, n);
        }
        StringBuilder sb = new StringBuilder();
        for (byte b : md.digest()) sb.append(String.format(Locale.ROOT, "%02x", b));
        return sb.toString();
    }

    static String exifDate(long ms, TimeZone tz) {
        SimpleDateFormat f = new SimpleDateFormat("yyyy:MM:dd HH:mm:ss", Locale.ROOT);
        f.setTimeZone(tz);
        return f.format(new Date(ms));
    }

    static long parseExif(String s, TimeZone tz) {
        SimpleDateFormat f = new SimpleDateFormat("yyyy:MM:dd HH:mm:ss", Locale.ROOT);
        f.setTimeZone(tz);
        try { return f.parse(s).getTime(); } catch (ParseException e) { return 0; }
    }

    static String offset(String taken, TimeZone tz) {
        long ms = parseExif(taken, tz);
        int off = tz.getOffset(ms > 0 ? ms : System.currentTimeMillis()) / 60000;
        char sign = off < 0 ? '-' : '+';
        off = Math.abs(off);
        return String.format(Locale.ROOT, "%c%02d:%02d", sign, off / 60, off % 60);
    }

    static final class BadRequest extends Exception { BadRequest(String m) { super(m); } }

    static final class Conflict extends Exception {
        final long have;
        Conflict(long have) { super("offset sbagliato"); this.have = have; }
    }
}
