package dumbphone.ponte.banco;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

// Test content for the Remoto bench (tools/remotonet), like the user's three benchmarks:
//   pagine  : Kindle. Pages of justified serif text on white; a tap on the right turns the page (250 ms slide),
//             on the left goes back. Still between turns: the stream must make the new page sharp and then stop.
//   gioco   : Pokemon in DraStic. Two DS screens (256x192 each, 3x, pixel art): a tile map that scrolls as if
//             walking (60 fps), a menu below. A tap changes the walking direction.
//   flipper : pinball. A fast ball with a trail over a dark playfield, flippers that move on a tap.
// A 24x24 marker in the top left corner changes color with every page / every 30 frames, so a decoded frame tells
// which state it shows.
public final class Banco extends Activity {
    private Scene scene;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        scene = new Scene(this, mode(getIntent()));
        setContentView(scene);
    }

    @Override
    protected void onNewIntent(Intent i) {
        super.onNewIntent(i);
        setIntent(i);   // a recreation (display off/on) must keep the new mode
        scene.setMode(mode(i));
    }

    private static String mode(Intent i) {
        String m = i == null ? null : i.getStringExtra("mode");
        return m == null ? "pagine" : m;
    }

    static final class Scene extends View {
        private String mode;
        private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.SUBPIXEL_TEXT_FLAG);
        private final Paint small = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint fill = new Paint();
        private int page;
        private long turnStart = -1;
        private int turnFrom, turnDir;
        private Bitmap tiles;          // the DS tile set, drawn once
        private Bitmap top, bottom;    // the two DS screens at native 256x192
        private final Rect src = new Rect(), dst = new Rect();
        private int walkX, walkY, dirX = 1, dirY = 0, frame;
        private float ballX = 300, ballY = 300, vx = 17, vy = 11;
        private final float[] trailX = new float[8], trailY = new float[8];
        private boolean flipL, flipR;

        Scene(Context c, String mode) {
            super(c);
            this.mode = mode;
            text.setTypeface(Typeface.SERIF);
            text.setColor(Color.BLACK);
            small.setColor(Color.DKGRAY);
            small.setTextSize(22);
        }

        void setMode(String m) {
            mode = m;
            invalidate();
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            if (e.getActionMasked() != MotionEvent.ACTION_DOWN) return true;
            if (mode.equals("pagine")) {
                if (turnStart >= 0) page = turnFrom + turnDir;   // a tap during a turn finishes it
                turnFrom = page;
                turnDir = e.getX() < getWidth() / 3f ? -1 : 1;
                if (page + turnDir < 0) return true;
                turnStart = SystemClock.uptimeMillis();
                invalidate();
            } else if (mode.equals("gioco")) {
                int nx = dirY, ny = -dirX;   // turn left
                dirX = nx;
                dirY = ny;
            } else {
                if (e.getX() < getWidth() / 2f) flipL = !flipL;
                else flipR = !flipR;
            }
            return true;
        }

        @Override
        protected void onDraw(Canvas c) {
            switch (mode) {
                case "gioco": drawGame(c); postInvalidateOnAnimation(); break;
                case "flipper": drawPinball(c); postInvalidateOnAnimation(); break;
                default: drawPages(c);
            }
            frame++;
        }

        // ------------------------------------------------------------------------------------------ pages

        private void drawPages(Canvas c) {
            int w = getWidth(), h = getHeight();
            if (turnStart < 0) {
                drawPage(c, page, 0, w, h);
                return;
            }
            float t = Math.min(1f, (SystemClock.uptimeMillis() - turnStart) / 250f);
            float ease = t * t * (3 - 2 * t);
            int shift = (int) (ease * w) * turnDir;
            drawPage(c, turnFrom, -shift, w, h);
            drawPage(c, turnFrom + turnDir, w * turnDir - shift, w, h);
            if (t >= 1f) {
                page = turnFrom + turnDir;
                turnStart = -1;
            }
            postInvalidateOnAnimation();
        }

        private void drawPage(Canvas c, int n, int dx, int w, int h) {
            c.save();
            c.translate(dx, 0);
            fill.setColor(Color.WHITE);
            c.drawRect(0, 0, w, h, fill);
            float size = h / 36f;            // ~35 px lines on 1280: Kindle's default size on this screen
            text.setTextSize(size);
            float margin = w * 0.07f, line = size * 1.45f, y = h * 0.06f + size;
            Random r = new Random(1000 + n);
            List<String> words = new ArrayList<>();
            while (y < h * 0.93f) {
                words.clear();
                float width = 0, space = text.measureText(" ");
                boolean paragraphEnd = r.nextInt(9) == 0;
                while (true) {
                    String word = WORDS[r.nextInt(WORDS.length)];
                    float ww = text.measureText(word);
                    if (width + ww > w - 2 * margin) break;
                    words.add(word);
                    width += ww + space;
                }
                if (paragraphEnd) {
                    words = words.subList(0, Math.max(1, words.size() / 2));
                    float x = margin;
                    for (String s : words) { c.drawText(s, x, y, text); x += text.measureText(s) + space; }
                    y += line * 1.4f;
                    continue;
                }
                float total = 0;
                for (String s : words) total += text.measureText(s);
                float gap = words.size() > 1 ? (w - 2 * margin - total) / (words.size() - 1) : 0, x = margin;
                for (String s : words) { c.drawText(s, x, y, text); x += text.measureText(s) + gap; }
                y += line;
            }
            String foot = "Pagina " + (n + 1) + " di 412          " + (n * 7 % 100) + "%";
            c.drawText(foot, margin, h * 0.975f, small);
            marker(c, n);
            c.restore();
        }

        private void marker(Canvas c, int n) {
            fill.setColor(Color.HSVToColor(new float[] {(n * 47) % 360, 1f, 1f}));
            c.drawRect(0, 0, 24, 24, fill);
        }

        // ------------------------------------------------------------------------------------------ DS game

        private void drawGame(Canvas c) {
            int w = getWidth(), h = getHeight();
            if (tiles == null) makeTiles();
            walkX += dirX * 2;
            walkY += dirY * 2;
            Canvas t = new Canvas(top);
            int ox = Math.floorMod(walkX, 16), oy = Math.floorMod(walkY, 16);
            int tx0 = Math.floorDiv(walkX, 16), ty0 = Math.floorDiv(walkY, 16);
            for (int ty = 0; ty < 14; ty++)
                for (int tx = 0; tx < 18; tx++) {
                    int kind = tileAt(tx0 + tx, ty0 + ty);
                    src.set(kind * 16, 0, kind * 16 + 16, 16);
                    dst.set(tx * 16 - ox, ty * 16 - oy, tx * 16 - ox + 16, ty * 16 - oy + 16);
                    t.drawBitmap(tiles, src, dst, null);
                }
            fill.setColor(Color.rgb(200, 40, 40));   // the player, with a walking bob
            int bob = (frame / 8) % 2;
            t.drawRect(120, 88 + bob, 136, 104 + bob, fill);
            fill.setColor(Color.rgb(250, 220, 180));
            t.drawRect(123, 84 + bob, 133, 92 + bob, fill);
            Canvas b = new Canvas(bottom);
            b.drawColor(Color.rgb(40, 60, 120));
            for (int i = 0; i < 4; i++) {
                fill.setColor(i == (frame / 60) % 4 ? Color.rgb(250, 200, 60) : Color.rgb(90, 120, 200));
                b.drawRect(16 + (i % 2) * 120, 24 + (i / 2) * 80, 128 + (i % 2) * 120, 92 + (i / 2) * 80, fill);
            }
            fill.setColor(Color.WHITE);
            b.drawRect(20, 176, 20 + (frame % 216), 184, fill);
            c.drawColor(Color.BLACK);
            int scale = Math.max(1, Math.min(w / 256, h / 384));
            int sw = 256 * scale, sh = 192 * scale, x0 = (w - sw) / 2, y0 = (h - 2 * sh) / 2;
            src.set(0, 0, 256, 192);
            dst.set(x0, y0, x0 + sw, y0 + sh);
            c.drawBitmap(top, src, dst, null);   // no filtering: pixel art
            dst.set(x0, y0 + sh, x0 + sw, y0 + 2 * sh);
            c.drawBitmap(bottom, src, dst, null);
            marker(c, frame / 30);
        }

        private int tileAt(int x, int y) {
            int hsh = x * 73856093 ^ y * 19349663;
            hsh ^= hsh >>> 13;
            int v = Math.floorMod(hsh, 100);
            return v < 55 ? 0 : v < 75 ? 1 : v < 88 ? 2 : v < 95 ? 3 : 4;   // grass, tall grass, path, tree, water
        }

        private void makeTiles() {
            tiles = Bitmap.createBitmap(80, 16, Bitmap.Config.ARGB_8888);
            int[][] colors = {{0x58b848, 0x48a038}, {0x389028, 0x50b040}, {0xd8c088, 0xc8b078}, {0x206018, 0x184810},
                              {0x4878e0, 0x6090f0}};
            Random r = new Random(7);
            for (int k = 0; k < 5; k++)
                for (int y = 0; y < 16; y++)
                    for (int x = 0; x < 16; x++) {
                        int col = colors[k][(r.nextInt(5) == 0 || (k == 3 && (x + y) % 5 == 0)) ? 1 : 0];
                        tiles.setPixel(k * 16 + x, y, 0xff000000 | col);
                    }
            top = Bitmap.createBitmap(256, 192, Bitmap.Config.ARGB_8888);
            bottom = Bitmap.createBitmap(256, 192, Bitmap.Config.ARGB_8888);
        }

        // ------------------------------------------------------------------------------------------ pinball

        private void drawPinball(Canvas c) {
            int w = getWidth(), h = getHeight();
            c.drawColor(Color.rgb(16, 20, 48));
            fill.setColor(Color.rgb(60, 70, 140));
            for (int i = 0; i < 6; i++) c.drawCircle(w * (0.2f + 0.3f * (i % 3)), h * (0.2f + 0.15f * (i / 3)), 40, fill);
            fill.setColor(Color.rgb(230, 230, 80));
            c.save();
            c.rotate(flipL ? -25 : 20, w * 0.2f, h * 0.85f);
            c.drawRect(w * 0.2f, h * 0.85f - 12, w * 0.45f, h * 0.85f + 12, fill);
            c.restore();
            c.save();
            c.rotate(flipR ? 25 : -20, w * 0.8f, h * 0.85f);
            c.drawRect(w * 0.55f, h * 0.85f - 12, w * 0.8f, h * 0.85f + 12, fill);
            c.restore();
            ballX += vx;
            ballY += vy;
            vy += 0.35f;
            if (ballX < 20 || ballX > w - 20) { vx = -vx; ballX = Math.max(20, Math.min(w - 20, ballX)); }
            if (ballY < 20) { vy = Math.abs(vy); }
            if (ballY > h - 20) { vy = -Math.max(18, Math.abs(vy)); ballY = h - 20; }
            for (int i = trailX.length - 1; i > 0; i--) { trailX[i] = trailX[i - 1]; trailY[i] = trailY[i - 1]; }
            trailX[0] = ballX;
            trailY[0] = ballY;
            for (int i = trailX.length - 1; i >= 0; i--) {
                fill.setColor(Color.argb(255 - i * 30, 220, 220, 230));
                c.drawCircle(trailX[i], trailY[i], 18 - i, fill);
            }
            small.setColor(Color.WHITE);
            small.setTextSize(48);
            c.drawText(String.valueOf(1000 + frame * 10), w * 0.35f, h * 0.06f, small);
            marker(c, frame / 30);
        }

        private static final String[] WORDS = ("il la che di un una per non con del della sul nella era come anche quando "
                + "poi dove casa notte giorno mare vento strada sempre ancora voce mano occhi cuore tempo storia luce "
                + "finestra lontano piano silenzio parole lettera camera porta inverno estate fiume città sguardo "
                + "pensava diceva sapeva guardava rimase tornò aspettava scrisse disse sembrava improvvisamente "
                + "mentre ormai forse davvero soltanto nessuno qualcosa niente tutto ogni altro stesso vecchio nuovo "
                + "grande piccolo ultimo primo bianco nero rosso freddo caldo lungo breve chiaro scuro").split(" ");
    }
}
