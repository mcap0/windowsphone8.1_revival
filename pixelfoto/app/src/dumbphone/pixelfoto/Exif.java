package dumbphone.pixelfoto;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.BufferedInputStream;
import java.io.BufferedOutputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * Rewrites a Lumia JPEG as a Pixel 2 XL photo: the Exif block of a real Pixel photo (assets/pixel-exif.tiff, with its
 * MakerNote) replaces the Lumia's APP0/APP1, with the Lumia's capture date, orientation, exposure and GPS position, the
 * real dimensions and a new thumbnail. The image data (DQT..EOI) is copied byte for byte.
 */
final class Exif {
    static final int IFD0 = 0, EXIF = 1, GPS = 2, IOP = 3, IFD1 = 4;
    private static final int P_EXIF = 0x8769, P_GPS = 0x8825, P_IOP = 0xA005, T_OFF = 0x0201, T_LEN = 0x0202;

    /** One entry; v is always little-endian. */
    static final class E {
        final int type, count;
        final byte[] v;
        E(int type, int count, byte[] v) { this.type = type; this.count = count; this.v = v; }
    }

    static final class Tiff {
        @SuppressWarnings("unchecked")
        final TreeMap<Integer, E>[] ifd = new TreeMap[5];
        byte[] thumb;
        Tiff() { for (int i = 0; i < 5; i++) ifd[i] = new TreeMap<>(); }
        E get(int i, int tag) { return ifd[i].get(tag); }
        String ascii(int i, int tag) {
            E e = get(i, tag);
            if (e == null || e.type != 2) return null;
            int n = 0;
            while (n < e.v.length && e.v[n] != 0) n++;
            return new String(e.v, 0, n, StandardCharsets.US_ASCII).trim();
        }
        int num(int i, int tag, int def) {
            E e = get(i, tag);
            if (e == null || e.v.length < 2) return def;
            if (e.type == 3) return (e.v[0] & 255) | (e.v[1] & 255) << 8;
            if (e.type == 4 && e.v.length >= 4) return (int) le32(e.v, 0);
            return def;
        }
        void ascii(int i, int tag, String s) {
            byte[] b = (s + "\0").getBytes(StandardCharsets.US_ASCII);
            ifd[i].put(tag, new E(2, b.length, b));
        }
        void shrt(int i, int tag, int x) { ifd[i].put(tag, new E(3, 1, new byte[]{(byte) x, (byte) (x >> 8)})); }
    }

    static int esize(int type) {
        switch (type) {
            case 3: case 8: return 2;
            case 4: case 9: case 11: case 13: return 4;
            case 5: case 10: case 12: return 8;
            default: return 1;
        }
    }

    private static int unit(int type) { return type == 5 || type == 10 ? 4 : esize(type); }

    static long le32(byte[] b, int p) {
        return (b[p] & 255L) | (b[p + 1] & 255L) << 8 | (b[p + 2] & 255L) << 16 | (b[p + 3] & 255L) << 24;
    }

    // ---- parsing ----

    static Tiff parse(byte[] b, int off, int len) {
        Tiff t = new Tiff();
        if (len < 8) return t;
        boolean le = b[off] == 'I';
        Rd r = new Rd(b, off, len, le);
        try {
            long ifd0 = r.u32(4);
            long next = readIfd(r, ifd0, t.ifd[IFD0]);
            E p = t.ifd[IFD0].remove(P_EXIF);
            if (p != null) {
                readIfd(r, le32(p.v, 0), t.ifd[EXIF]);
                E q = t.ifd[EXIF].remove(P_IOP);
                if (q != null) readIfd(r, le32(q.v, 0), t.ifd[IOP]);
            }
            p = t.ifd[IFD0].remove(P_GPS);
            if (p != null) readIfd(r, le32(p.v, 0), t.ifd[GPS]);
            if (next != 0) {
                readIfd(r, next, t.ifd[IFD1]);
                E o = t.ifd[IFD1].remove(T_OFF), l = t.ifd[IFD1].remove(T_LEN);
                if (o != null && l != null) {
                    long to = le32(o.v, 0), tl = le32(l.v, 0);
                    if (to + tl <= len) {
                        t.thumb = new byte[(int) tl];
                        System.arraycopy(b, off + (int) to, t.thumb, 0, (int) tl);
                    }
                }
            }
        } catch (RuntimeException ignored) {
            // A damaged block: keep whatever was read.
        }
        return t;
    }

    private static final class Rd {
        final byte[] b; final int off, len; final boolean le;
        Rd(byte[] b, int off, int len, boolean le) { this.b = b; this.off = off; this.len = len; this.le = le; }
        int u8(long p) {
            if (p < 0 || p >= len) throw new IndexOutOfBoundsException();
            return b[off + (int) p] & 255;
        }
        int u16(long p) { return le ? u8(p) | u8(p + 1) << 8 : u8(p) << 8 | u8(p + 1); }
        long u32(long p) {
            return le ? (u16(p) & 0xFFFFL) | (long) u16(p + 2) << 16 : (long) u16(p) << 16 | (u16(p + 2) & 0xFFFFL);
        }
    }

    private static long readIfd(Rd r, long at, TreeMap<Integer, E> into) {
        int n = r.u16(at);
        if (n > 1000) throw new IllegalStateException();
        for (int i = 0; i < n; i++) {
            long e = at + 2 + 12L * i;
            int tag = r.u16(e), type = r.u16(e + 2);
            long count = r.u32(e + 4);
            long size = esize(type) * count;
            if (size > r.len) continue;
            long data = size <= 4 ? e + 8 : r.u32(e + 8);
            byte[] v = new byte[(int) size];
            for (int k = 0; k < size; k++) v[k] = (byte) r.u8(data + k);
            int u = unit(type);
            if (!r.le && u > 1) {
                for (int k = 0; k + u <= v.length; k += u)
                    for (int a = k, z = k + u - 1; a < z; a++, z--) { byte x = v[a]; v[a] = v[z]; v[z] = x; }
            }
            into.put(tag, new E(type, (int) count, v));
        }
        return r.u32(at + 2 + 12L * n);
    }

    // ---- writing (little-endian, like the Pixel) ----

    private static int ifdSize(Map<Integer, E> m) {
        int s = 2 + 12 * m.size() + 4;
        for (E e : m.values()) if (e.v.length > 4) s += (e.v.length + 1) & ~1;
        return s;
    }

    private static E long1(long x) {
        return new E(4, 1, new byte[]{(byte) x, (byte) (x >> 8), (byte) (x >> 16), (byte) (x >> 24)});
    }

    static byte[] write(Tiff t) {
        TreeMap<Integer, E> i0 = new TreeMap<>(t.ifd[IFD0]), ex = new TreeMap<>(t.ifd[EXIF]),
                gps = new TreeMap<>(t.ifd[GPS]), iop = new TreeMap<>(t.ifd[IOP]), i1 = new TreeMap<>(t.ifd[IFD1]);
        boolean hasGps = !gps.isEmpty(), hasIop = !iop.isEmpty(), hasThumb = t.thumb != null;
        E zero = long1(0);
        i0.put(P_EXIF, zero);
        if (hasGps) i0.put(P_GPS, zero);
        if (hasIop) ex.put(P_IOP, zero);
        if (hasThumb) { i1.put(T_OFF, zero); i1.put(T_LEN, zero); }
        int o0 = 8, oEx = o0 + ifdSize(i0), oIop = oEx + ifdSize(ex);
        int oGps = oIop + (hasIop ? ifdSize(iop) : 0);
        int o1 = oGps + (hasGps ? ifdSize(gps) : 0);
        int oThumb = o1 + (hasThumb ? ifdSize(i1) : 0);
        i0.put(P_EXIF, long1(oEx));
        if (hasGps) i0.put(P_GPS, long1(oGps));
        if (hasIop) ex.put(P_IOP, long1(oIop));
        if (hasThumb) { i1.put(T_OFF, long1(oThumb)); i1.put(T_LEN, long1(t.thumb.length)); }
        int total = oThumb + (hasThumb ? t.thumb.length : 0);
        byte[] out = new byte[total];
        out[0] = 'I'; out[1] = 'I'; out[2] = 42; out[4] = 8;
        put(out, o0, i0, hasThumb ? o1 : 0);
        put(out, oEx, ex, 0);
        if (hasIop) put(out, oIop, iop, 0);
        if (hasGps) put(out, oGps, gps, 0);
        if (hasThumb) {
            put(out, o1, i1, 0);
            System.arraycopy(t.thumb, 0, out, oThumb, t.thumb.length);
        }
        return out;
    }

    private static void w16(byte[] b, int p, int x) { b[p] = (byte) x; b[p + 1] = (byte) (x >> 8); }
    private static void w32(byte[] b, int p, long x) { w16(b, p, (int) x); w16(b, p + 2, (int) (x >> 16)); }

    private static void put(byte[] b, int at, Map<Integer, E> m, int next) {
        w16(b, at, m.size());
        int data = at + 2 + 12 * m.size() + 4, i = 0;
        for (Map.Entry<Integer, E> en : m.entrySet()) {
            E e = en.getValue();
            int p = at + 2 + 12 * i++;
            w16(b, p, en.getKey());
            w16(b, p + 2, e.type);
            w32(b, p + 4, e.count);
            if (e.v.length <= 4) System.arraycopy(e.v, 0, b, p + 8, e.v.length);
            else {
                w32(b, p + 8, data);
                System.arraycopy(e.v, 0, b, data, e.v.length);
                data += (e.v.length + 1) & ~1;
            }
        }
        w32(b, at + 2 + 12 * m.size(), next);
    }

    // ---- JPEG ----

    static final class Seg {
        final int marker; final byte[] data;
        Seg(int marker, byte[] data) { this.marker = marker; this.data = data; }
    }

    /** Header segments up to (not including) SOS; the stream is left at the SOS marker's length field. */
    static List<Seg> readHeader(DataInputStream in) throws IOException {
        if (in.readUnsignedShort() != 0xFFD8) throw new IOException("non è un JPEG");
        List<Seg> segs = new ArrayList<>();
        while (true) {
            int m = in.readUnsignedByte();
            if (m != 0xFF) throw new IOException("JPEG rovinato");
            m = in.readUnsignedByte();
            while (m == 0xFF) m = in.readUnsignedByte();
            if (m == 0xDA) { segs.add(new Seg(m, null)); return segs; }
            if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) continue;
            int len = in.readUnsignedShort();
            byte[] d = new byte[len - 2];
            in.readFully(d);
            segs.add(new Seg(m, d));
        }
    }

    static boolean isExif(Seg s) {
        return s.marker == 0xE1 && s.data.length > 6 && s.data[0] == 'E' && s.data[1] == 'x' && s.data[2] == 'i'
                && s.data[3] == 'f' && s.data[4] == 0 && s.data[5] == 0;
    }

    static final class Result { String taken; int width, height; }

    /**
     * Writes `out` from `in`. `fallbackTaken` ("yyyy:MM:dd HH:mm:ss") is used when the Lumia file has no date;
     * `offset` is the local UTC offset for that date ("+01:00"); `subsec` 6 digits.
     */
    static Result rewrite(File in, File out, byte[] template, String fallbackTaken, OffsetFn offset, String subsec)
            throws IOException {
        Result res = new Result();
        try (DataInputStream src = new DataInputStream(new BufferedInputStream(new FileInputStream(in), 1 << 16))) {
            List<Seg> segs = readHeader(src);
            Tiff lumia = new Tiff();
            for (Seg s : segs) {
                if (s.data != null && isExif(s)) { lumia = parse(s.data, 6, s.data.length - 6); break; }
            }
            for (Seg s : segs) {
                if (s.data == null) continue;
                int m = s.marker;
                if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && s.data.length >= 5) {
                    res.height = (s.data[1] & 255) << 8 | (s.data[2] & 255);
                    res.width = (s.data[3] & 255) << 8 | (s.data[4] & 255);
                }
            }
            if (res.width == 0) throw new IOException("JPEG senza dimensioni");

            Tiff t = parse(template, 0, template.length);
            String taken = lumia.ascii(EXIF, 0x9003);
            if (taken == null || taken.length() != 19 || taken.startsWith("0000")) taken = lumia.ascii(IFD0, 0x0132);
            if (taken == null || taken.length() != 19 || taken.startsWith("0000")) taken = fallbackTaken;
            res.taken = taken;
            String off = offset.at(taken);

            t.ascii(IFD0, 0x0132, taken);
            t.ascii(EXIF, 0x9003, taken);
            t.ascii(EXIF, 0x9004, taken);
            for (int tag : new int[]{0x9010, 0x9011, 0x9012}) t.ascii(EXIF, tag, off);
            for (int tag : new int[]{0x9290, 0x9291, 0x9292}) t.ascii(EXIF, tag, subsec);
            t.shrt(IFD0, 0x0100, res.width);
            t.shrt(IFD0, 0x0101, res.height);
            t.shrt(EXIF, 0xA002, res.width);
            t.shrt(EXIF, 0xA003, res.height);
            int orient = lumia.num(IFD0, 0x0112, 1);
            if (orient < 1 || orient > 8) orient = 1;
            t.shrt(IFD0, 0x0112, orient);
            // What the Lumia measured: exposure, ISO, aperture, flash.
            for (int tag : new int[]{0x829A, 0x829D, 0x8827, 0x9201, 0x9202, 0x9204, 0x9209}) {
                E e = lumia.get(EXIF, tag);
                E mine = t.get(EXIF, tag);
                if (e != null && mine != null && e.type == mine.type && e.count == 1) t.ifd[EXIF].put(tag, e);
            }
            // GPS position when the Lumia had one.
            if (lumia.get(GPS, 0x0002) != null && lumia.get(GPS, 0x0004) != null) {
                t.ifd[GPS].clear();
                t.ifd[GPS].putAll(lumia.ifd[GPS]);
            }
            t.thumb = thumbnail(in, res.width, res.height);
            if (t.thumb != null) {
                BitmapFactory.Options bo = new BitmapFactory.Options();
                bo.inJustDecodeBounds = true;
                BitmapFactory.decodeByteArray(t.thumb, 0, t.thumb.length, bo);
                t.shrt(IFD1, 0x0100, bo.outWidth);
                t.shrt(IFD1, 0x0101, bo.outHeight);
                t.shrt(IFD1, 0x0112, orient);
            } else {
                t.ifd[IFD1].clear();
            }
            byte[] tiff = write(t);
            if (t.thumb != null && tiff.length + 8 > 65535) { t.thumb = null; t.ifd[IFD1].clear(); tiff = write(t); }
            if (tiff.length + 8 > 65535) throw new IOException("Exif troppo grande");

            try (OutputStream o = new BufferedOutputStream(new FileOutputStream(out), 1 << 16)) {
                o.write(0xFF); o.write(0xD8);
                int l = tiff.length + 8;
                o.write(0xFF); o.write(0xE1); o.write(l >> 8); o.write(l & 255);
                o.write(new byte[]{'E', 'x', 'i', 'f', 0, 0});
                o.write(tiff);
                for (Seg s : segs) {
                    if (s.data == null) break;
                    if (s.marker == 0xE0 || s.marker == 0xE1) continue; // JFIF, Lumia Exif, XMP
                    o.write(0xFF); o.write(s.marker);
                    int sl = s.data.length + 2;
                    o.write(sl >> 8); o.write(sl & 255);
                    o.write(s.data);
                }
                o.write(0xFF); o.write(0xDA);
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = src.read(buf)) > 0) o.write(buf, 0, n);
            }
        }
        return res;
    }

    interface OffsetFn { String at(String taken); }

    /** Like the Pixel's: longest side 252, stored unrotated (the Orientation tag applies). */
    static byte[] thumbnail(File f, int w, int h) {
        try {
            int longest = Math.max(w, h), sample = 1;
            while (longest / (sample * 2) >= 252) sample *= 2;
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inSampleSize = sample;
            Bitmap b = BitmapFactory.decodeFile(f.getPath(), o);
            if (b == null) return null;
            float k = 252f / Math.max(b.getWidth(), b.getHeight());
            Bitmap s = Bitmap.createScaledBitmap(b, Math.max(1, Math.round(b.getWidth() * k)),
                    Math.max(1, Math.round(b.getHeight() * k)), true);
            ByteArrayOutputStream bo = new ByteArrayOutputStream();
            s.compress(Bitmap.CompressFormat.JPEG, 85, bo);
            if (s != b) s.recycle();
            b.recycle();
            return bo.toByteArray();
        } catch (Throwable e) {
            return null;
        }
    }

    static byte[] readAll(InputStream in) throws IOException {
        ByteArrayOutputStream bo = new ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
        return bo.toByteArray();
    }

    private Exif() {}
}
