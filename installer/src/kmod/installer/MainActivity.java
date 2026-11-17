package kmod.installer;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.Comparator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** kmod installer: pick an installed Unreal game, check it, inject the kmod loader, install the result. */
public class MainActivity extends Activity {
    private static final String ACTION_RESULT = "kmod.installer.INSTALL_RESULT";
    private static final int REQ_UNINSTALL = 7;
    private static final int BG = 0xFF161B22, CARD = 0xFF222B36, TEXT = 0xFFE6EDF3, DIM = 0xFF8B98A5, ACCENT = 0xFF4FC3A1, WARN = 0xFFE3B341, ERR = 0xFFF47067;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private LinearLayout root;
    private static final class App { String label, pkg, path; boolean split; }
    private App current;                    // the app being worked on
    private File output;                    // the injected APK
    private Keys keys;
    private boolean busy;

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().setStatusBarColor(BG); getWindow().setNavigationBarColor(BG);
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(BG);
        root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(24), dp(18), dp(24));
        sv.addView(root, new ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(sv);
        IntentFilter f = new IntentFilter(ACTION_RESULT);
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(receiver, f, Context.RECEIVER_NOT_EXPORTED); else registerReceiver(receiver, f);
        showList();
        cleanOldOutputs();
        showCreditOnce();
    }

    /** 首次启动弹出原作者与协议声明（只弹一次） */
    private void showCreditOnce() {
        try {
            final android.content.SharedPreferences sp = getSharedPreferences("kmod_installer", MODE_PRIVATE);
            boolean done = sp.getBoolean("credit_shown", false);
            android.util.Log.d("kmodinstaller", "showCreditOnce done=" + done);
            if (done) return;
            new AlertDialog.Builder(this)
                .setTitle("声明")
                .setMessage("原作者：KardsWalker卡兹走路人 · MIT License\n\n基于 CCB-TEAM/KMod-Loader-Android 修改构建，仅在本私服内部分发。")
                .setPositiveButton("知道了", new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface d, int w) {
                    sp.edit().putBoolean("credit_shown", true).apply();
                } })
                .setCancelable(false)
                .show();
        } catch (Throwable t) {
            android.util.Log.e("kmodinstaller", "credit dialog failed", t);
        }
    }

    /** The injected APKs are 1+ GB: remove the ones a previous run left behind (installed, cancelled or failed). */
    private void cleanOldOutputs() {
        new Thread(new Runnable() { public void run() {
            File[] dirs = getExternalMediaDirs();
            if (dirs.length == 0 || dirs[0] == null) return;
            File[] files = dirs[0].listFiles();
            if (files == null) return;
            for (File f : files) if (f.getName().endsWith("-kmod.apk") && !busy && (output == null || !f.equals(output))) f.delete();
        } }).start();
    }

    @Override protected void onDestroy() { super.onDestroy(); unregisterReceiver(receiver); }
    @Override public void onBackPressed() { if (current != null && !busy) { current = null; showList(); } else if (!busy) super.onBackPressed(); }

    // ---------------------------------------------------------------------------------------------- views
    private int dp(float v) { return Math.round(v * getResources().getDisplayMetrics().density); }

    private TextView text(String s, float size, int color, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s); t.setTextSize(size); t.setTextColor(color);
        if (bold) t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private LinearLayout.LayoutParams lp(int top) {
        LinearLayout.LayoutParams p = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        p.topMargin = dp(top);
        return p;
    }

    private View button(String label, int color, View.OnClickListener l) {
        TextView t = text(label, 16, 0xFF0D1117, true);
        t.setGravity(Gravity.CENTER);
        t.setPadding(dp(16), dp(13), dp(16), dp(13));
        GradientDrawable g = new GradientDrawable(); g.setColor(color); g.setCornerRadius(dp(10));
        t.setBackground(g); t.setOnClickListener(l);
        return t;
    }

    private LinearLayout card() {
        LinearLayout c = new LinearLayout(this);
        c.setOrientation(LinearLayout.VERTICAL);
        c.setPadding(dp(14), dp(12), dp(14), dp(12));
        GradientDrawable g = new GradientDrawable(); g.setColor(CARD); g.setCornerRadius(dp(12));
        c.setBackground(g);
        return c;
    }

    private void header(String title, String sub) {
        root.removeAllViews();
        root.addView(text(title, 24, TEXT, true));
        if (sub != null) { TextView s = text(sub, 13, DIM, false); root.addView(s, lp(4)); }
    }

    // ---------------------------------------------------------------------------------------------- screen 1: the games
    private void showList() {
        current = null;
        header("KARDS 美化安装器", "仅在 CAP113 私服分发");
        root.addView(text("基于 CCB-TEAM/KMod-Loader-Android（MIT）", 11, DIM, false), lp(2));
        final TextView status = text("正在查找已安装的虚幻引擎游戏…", 14, DIM, false);
        root.addView(status, lp(16));
        new Thread(new Runnable() { public void run() {
            final List<App> apps = scan();
            ui.post(new Runnable() { public void run() {
                if (current != null) return;
                root.removeView(status);
                if (apps.isEmpty()) root.addView(text("没有找到 64 位的虚幻引擎游戏。", 14, DIM, false), lp(16));
                for (final App a : apps) {
                    LinearLayout c = card();
                    c.addView(text(a.label, 17, TEXT, true));
                    c.addView(text(a.pkg + (a.split ? "\n分包安装的应用暂不支持" : ""), 12, a.split ? WARN : DIM, false));
                    c.setOnClickListener(new View.OnClickListener() { public void onClick(View v) { showDetail(a); } });
                    root.addView(c, lp(10));
                }
                root.addView(button("重新查找", ACCENT, new View.OnClickListener() { public void onClick(View v) { showList(); } }), lp(18));
                root.addView(text("注入后的应用用安装器生成的密钥重新签名，需要先卸载原版才能安装。", 12, DIM, false), lp(12));
            } });
        } }).start();
    }

    private List<App> scan() {
        PackageManager pm = getPackageManager();
        List<App> out = new ArrayList<>();
        for (ApplicationInfo ai : pm.getInstalledApplications(0)) {
            if ((ai.flags & ApplicationInfo.FLAG_SYSTEM) != 0 || ai.packageName.equals(getPackageName())) continue;
            if (ai.sourceDir == null || !Inspect.looksLikeUnreal(new File(ai.sourceDir))) continue;
            App a = new App();
            a.label = String.valueOf(pm.getApplicationLabel(ai)); a.pkg = ai.packageName; a.path = ai.sourceDir;
            a.split = ai.splitSourceDirs != null && ai.splitSourceDirs.length > 0;
            out.add(a);
        }
        Collections.sort(out, new Comparator<App>() { public int compare(App x, App y) { return x.label.compareToIgnoreCase(y.label); } });
        return out;
    }

    // ---------------------------------------------------------------------------------------------- screen 2: one game
    private void showDetail(final App a) {
        current = a; output = null;
        header(a.label, a.pkg);
        final TextView checking = text("正在检查…", 14, DIM, false);
        root.addView(checking, lp(16));
        new Thread(new Runnable() { public void run() {
            final Inspect.Report r = Inspect.analyze(new File(a.path), true);
            ui.post(new Runnable() { public void run() {
                if (current != a) return;
                root.removeView(checking);
                LinearLayout c = card();
                for (Map.Entry<String, String> e : r.facts.entrySet()) c.addView(text(e.getKey() + "：" + e.getValue(), 14, TEXT, false), lp(4));
                root.addView(c, lp(14));
                if (a.split) r.error("这个应用是分包安装的，暂不支持");
                for (String w : r.warnings) root.addView(text("⚠ " + w, 13, WARN, false), lp(10));
                for (String e : r.errors) root.addView(text("✖ " + e, 13, ERR, false), lp(10));
                if (r.ok && !a.split) {
                    root.addView(text("可以注入。", 15, ACCENT, true), lp(16));
                    root.addView(button("注入并安装", ACCENT, new View.OnClickListener() { public void onClick(View v) { startInject(a); } }), lp(12));
                } else root.addView(text("不能注入这个应用。", 15, ERR, true), lp(16));
                root.addView(button("导入美化包（.pak）", 0xFF3E7CB1, new View.OnClickListener() { public void onClick(View v) { importPak(a); } }), lp(12));
                root.addView(text("把 .pak 复制到 " + a.pkg + " 的 kmod/paks/，游戏启动时自动挂载。", 11, DIM, false), lp(6));
                root.addView(button("管理美化包（查看 / 删除）", 0xFF6E7681, new View.OnClickListener() { public void onClick(View v) { showMods(a); } }), lp(10));
                root.addView(button("返回", 0xFF8B98A5, new View.OnClickListener() { public void onClick(View v) { showList(); } }), lp(10));
            } });
        } }).start();
    }

    // ---------------------------------------------------------------------------------------------- inject
    private TextView progressText;

    private void setBusyView(String title) {
        busy = true;
        header(title, current.pkg);
        root.addView(new ProgressBar(this), lp(24));
        progressText = text("", 14, TEXT, false);
        root.addView(progressText, lp(12));
    }

    private void progress(final String s) { ui.post(new Runnable() { public void run() { if (progressText != null) progressText.setText(s); } }); }

    private void startInject(final App a) {
        if (Build.VERSION.SDK_INT >= 26 && !getPackageManager().canRequestPackageInstalls()) {
            new AlertDialog.Builder(this).setMessage("需要先允许这个应用安装其他应用。点“去设置”打开开关，然后回来重试。")
                .setPositiveButton("去设置", new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface d, int w) {
                    startActivity(new Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES, Uri.parse("package:" + getPackageName()))); } })
                .setNegativeButton("取消", null).show();
            return;
        }
        setBusyView("正在注入 " + a.label);
        new Thread(new Runnable() { public void run() {
            try {
                // 优先使用随包预置的签名密钥（与私服客户端同源）
                try { Keys.bundledPk8 = readAsset("signing.pk8"); Keys.bundledCrt = readAsset("signing.crt"); } catch (Throwable ignored) { }
                File media = getExternalMediaDirs().length > 0 && getExternalMediaDirs()[0] != null ? new File(getExternalMediaDirs()[0], "signing") : null;
                keys = Keys.loadOrCreate(new File(getFilesDir(), "keys"), media);
                Map<String, byte[]> libs = new LinkedHashMap<>();
                for (String n : new String[]{"libkmod.so", "libkmodcore.so"}) libs.put(n, readAsset(n));
                File[] media0 = getExternalMediaDirs();                      // the result is large: keep it on shared storage (visible in a file manager)
                File outDir = media0.length > 0 && media0[0] != null ? media0[0] : getCacheDir();
                File out = new File(outDir, a.pkg + "-kmod.apk");
                Injector.run(new File(a.path), out, libs, keys, new File(getCacheDir(), "work"), new Injector.Progress() { public void step(String t) { progress(t); } });
                output = out;
                ui.post(new Runnable() { public void run() { afterInject(); } });
            } catch (final Throwable t) {
                t.printStackTrace();
                ui.post(new Runnable() { public void run() { fail("注入失败：" + t); } });
            }
        } }).start();
    }

    private byte[] readAsset(String name) throws Exception {
        try (InputStream in = getAssets().open(name)) {
            java.io.ByteArrayOutputStream o = new java.io.ByteArrayOutputStream(); byte[] b = new byte[1 << 16]; int n;
            while ((n = in.read(b)) > 0) o.write(b, 0, n);
            return o.toByteArray();
        }
    }

    private void fail(String msg) {
        busy = false;
        header("出错了", current != null ? current.pkg : null);
        root.addView(text(msg, 14, ERR, false), lp(16));
        root.addView(button("返回", 0xFF8B98A5, new View.OnClickListener() { public void onClick(View v) { showList(); } }), lp(18));
    }

    // ---------------------------------------------------------------------------------------------- uninstall the original, then install
    private boolean installed(String pkg) {
        try { getPackageManager().getPackageInfo(pkg, 0); return true; } catch (PackageManager.NameNotFoundException e) { return false; }
    }

    /** Is the installed app already signed with our key? Then it can be updated in place, without uninstalling. */
    private boolean signedByUs(String pkg) {
        try {
            PackageInfo pi = getPackageManager().getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
            if (pi.signingInfo == null) return false;
            for (Signature s : pi.signingInfo.getApkContentsSigners())
                if (java.util.Arrays.equals(s.toByteArray(), keys.cert.getEncoded())) return true;
        } catch (Exception e) { /* not installed */ }
        return false;
    }

    private void afterInject() {
        progress("注入完成：" + (output.length() / (1024 * 1024)) + " MB");
        if (installed(current.pkg) && !signedByUs(current.pkg)) {
            new AlertDialog.Builder(this).setCancelable(false)
                .setMessage("原版应用的签名和注入后的不同，必须先卸载原版。卸载会删除它的应用数据（账号通常在服务器上；kmod 的模组文件夹不受影响）。\n\n点“卸载”后在系统弹窗里确认。")
                .setPositiveButton("卸载", new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface d, int w) {
                    Intent i = new Intent(Intent.ACTION_DELETE, Uri.parse("package:" + current.pkg));
                    i.putExtra(Intent.EXTRA_RETURN_RESULT, true);
                    startActivityForResult(i, REQ_UNINSTALL); } })
                .setNegativeButton("取消", new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface d, int w) { fail("已取消。注入后的安装包保存在 " + output.getAbsolutePath() + "，原版没有改动。"); } })
                .show();
        } else install();
    }

    @Override protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_PAK) {
            if (res == RESULT_OK && data != null && data.getData() != null && current != null) copyPak(current, data.getData());
            return;
        }
        if (req != REQ_UNINSTALL || current == null) return;
        if (installed(current.pkg)) fail("原版还没有卸载，所以没有安装。"); else install();
    }

    // ---------------------------------------------------------------------------------------------- import a .pak
    private static final int REQ_PAK = 9;

    private File pakDir(String pkg) {
        return new File(Environment.getExternalStorageDirectory(), "Android/media/" + pkg + "/kmod/paks");
    }

    private void importPak(final App a) {
        if (!ensureAllFiles(a.pkg)) return;
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        i.putExtra(Intent.EXTRA_MIME_TYPES, new String[]{"application/octet-stream", "*/*"});
        try { startActivityForResult(i, REQ_PAK); } catch (Throwable t) { fail("打不开文件选择器：" + t); }
    }

    /** 需要「所有文件访问权限」才能写别的应用的 Android/media 目录 */
    private boolean ensureAllFiles(String pkg) {
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            new AlertDialog.Builder(this)
                .setMessage("这个操作需要「所有文件访问权限」，用来读写 " + pkg + " 的 kmod 目录。\n\n点“去设置”→ 允许本应用访问所有文件 → 回来重试。")
                .setPositiveButton("去设置", new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface d, int w) {
                    try {
                        startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())));
                    } catch (Throwable t) {
                        startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
                    }
                } })
                .setNegativeButton("取消", null).show();
            return false;
        }
        return true;
    }

    private File modsDir(String pkg) {
        return new File(Environment.getExternalStorageDirectory(), "Android/media/" + pkg + "/kmod/mods");
    }

    private static String humanSize(long n) {
        if (n >= 1024L * 1024 * 1024) return String.format("%.2f GB", n / 1073741824.0);
        if (n >= 1024L * 1024) return String.format("%.1f MB", n / 1048576.0);
        if (n >= 1024) return String.format("%.1f KB", n / 1024.0);
        return n + " B";
    }

    private static long sizeOf(File f) {
        if (f.isFile()) return f.length();
        long s = 0;
        File[] l = f.listFiles();
        if (l != null) for (File c : l) s += sizeOf(c);
        return s;
    }

    private static boolean deleteRec(File f) {
        if (f.isDirectory()) {
            File[] l = f.listFiles();
            if (l != null) for (File c : l) deleteRec(c);
        }
        return f.delete();
    }

    // ---------------------------------------------------------------------------------------------- screen 3: manage mods
    private void showMods(final App a) {
        if (!ensureAllFiles(a.pkg)) return;
        header("管理美化包", a.pkg);
        root.addView(text("pak 目录：kmod/paks/　　mod 目录：kmod/mods/", 11, DIM, false), lp(6));

        final File paks = pakDir(a.pkg), mods = modsDir(a.pkg);
        int count = 0;

        File[] pf = paks.listFiles();
        if (pf != null && pf.length > 0) {
            root.addView(text("美化包（.pak）", 15, ACCENT, true), lp(14));
            for (final File f : pf) {
                if (f.isDirectory()) continue;
                count++;
                LinearLayout c = card();
                c.addView(text(f.getName(), 15, TEXT, true));
                c.addView(text(humanSize(f.length()), 12, DIM, false));
                LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL);
                row.addView(button("删除", 0xFFF47067, new View.OnClickListener() { public void onClick(View v) { confirmDelete(a, f, true); } }), lp(8));
                c.addView(row);
                root.addView(c, lp(10));
            }
        }

        File[] mf = mods.listFiles();
        if (mf != null && mf.length > 0) {
            root.addView(text("模组（mods/）", 15, ACCENT, true), lp(14));
            for (final File f : mf) {
                count++;
                LinearLayout c = card();
                c.addView(text(f.getName(), 15, TEXT, true));
                c.addView(text((f.isDirectory() ? "文件夹" : "文件") + "　" + humanSize(sizeOf(f)), 12, DIM, false));
                LinearLayout row = new LinearLayout(this); row.setOrientation(LinearLayout.HORIZONTAL);
                row.addView(button("删除", 0xFFF47067, new View.OnClickListener() { public void onClick(View v) { confirmDelete(a, f, false); } }), lp(8));
                c.addView(row);
                root.addView(c, lp(10));
            }
        }

        if (count == 0) {
            root.addView(text("目录里还没有美化包或模组。", 14, DIM, false), lp(16));
            root.addView(text(paks.getAbsolutePath(), 10, DIM, false), lp(6));
        }
        root.addView(button("返回", 0xFF8B98A5, new View.OnClickListener() { public void onClick(View v) { showDetail(a); } }), lp(10));
    }

    private void confirmDelete(final App a, final File f, final boolean isPak) {
        new AlertDialog.Builder(this)
            .setMessage("删除 " + f.getName() + " ？\n\n" + (isPak ? "删除后该美化包下次启动不再挂载。" : "删除后该模组不再加载。"))
            .setPositiveButton("删除", new android.content.DialogInterface.OnClickListener() { public void onClick(android.content.DialogInterface d, int w) {
                final boolean ok = deleteRec(f);
                ui.post(new Runnable() { public void run() {
                    toast(ok ? "已删除：" + f.getName() : "删除失败（权限不足？）");
                    showMods(a);
                } });
            } })
            .setNegativeButton("取消", null).show();
    }

    private void copyPak(final App a, final Uri uri) {
        setBusyView("正在导入美化包");
        new Thread(new Runnable() { public void run() {
            String name = "mod.pak";
            try {
                String dn = null;
                android.database.Cursor c = getContentResolver().query(uri, null, null, null, null);
                if (c != null) { try { if (c.moveToFirst()) { int idx = c.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME); if (idx >= 0) dn = c.getString(idx); } } finally { c.close(); } }
                if (dn != null && dn.length() > 0) name = dn;
                if (!name.toLowerCase().endsWith(".pak")) name = name + ".pak";
                File dir = pakDir(a.pkg);
                if (!dir.exists() && !dir.mkdirs()) throw new RuntimeException("无法创建目录 " + dir);
                File out = new File(dir, name);
                try (InputStream in = getContentResolver().openInputStream(uri); OutputStream os = new java.io.FileOutputStream(out)) {
                    if (in == null) throw new RuntimeException("读不到文件");
                    byte[] b = new byte[1 << 16]; int n; long total = 0;
                    while ((n = in.read(b)) > 0) { os.write(b, 0, n); total += n; }
                    progress("已写入 " + (total / 1024 / 1024) + " MB");
                }
                final String msg = "导入完成：" + name + "\n位置：" + out.getAbsolutePath();
                ui.post(new Runnable() { public void run() { busy = false; showList(); toast(msg); } });
            } catch (final Throwable t) {
                t.printStackTrace();
                ui.post(new Runnable() { public void run() { busy = false; fail("导入失败：" + t); } });
            }
        } }).start();
    }

    private void toast(String s) {
        android.widget.Toast.makeText(this, s, android.widget.Toast.LENGTH_LONG).show();
    }

    private void install() {
        progress("正在安装…（系统会弹出确认窗口）");
        new Thread(new Runnable() { public void run() {
            try {
                PackageInstaller pi = getPackageManager().getPackageInstaller();
                PackageInstaller.SessionParams sp = new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
                sp.setAppPackageName(current.pkg);
                int id = pi.createSession(sp);
                try (PackageInstaller.Session s = pi.openSession(id)) {
                    try (OutputStream os = s.openWrite("base.apk", 0, output.length()); InputStream in = new FileInputStream(output)) {
                        byte[] buf = new byte[1 << 16]; int n;
                        while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                        s.fsync(os);
                    }
                    Intent i = new Intent(ACTION_RESULT).setPackage(getPackageName());
                    PendingIntent pend = PendingIntent.getBroadcast(MainActivity.this, id, i, PendingIntent.FLAG_MUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
                    s.commit(pend.getIntentSender());
                }
            } catch (final Throwable t) { ui.post(new Runnable() { public void run() { fail("安装失败：" + t); } }); }
        } }).start();
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override public void onReceive(Context c, Intent i) {
            int status = i.getIntExtra(PackageInstaller.EXTRA_STATUS, -1);
            String msg = i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE);
            if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
                Intent confirm = (Intent) i.getParcelableExtra(Intent.EXTRA_INTENT);
                if (confirm != null) { confirm.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK); startActivity(confirm); }
            } else if (status == PackageInstaller.STATUS_SUCCESS) {
                done();
            } else {
                fail("系统拒绝了安装（" + status + "）：" + msg);
            }
        }
    };

    private void done() {
        busy = false;
        final String pkg = current.pkg;
        header("安装完成", pkg);
        root.addView(text("kmod 已经注入。第一次启动游戏时，kmod 会在手机上建立它的文件夹：\nAndroid/media/" + pkg + "/kmod", 14, TEXT, false), lp(16));
        root.addView(button("打开游戏", ACCENT, new View.OnClickListener() { public void onClick(View v) {
            Intent l = getPackageManager().getLaunchIntentForPackage(pkg);
            if (l != null) startActivity(l); } }), lp(18));
        root.addView(button("返回", 0xFF8B98A5, new View.OnClickListener() { public void onClick(View v) { showList(); } }), lp(10));
        if (output != null) output.delete();
    }
}
