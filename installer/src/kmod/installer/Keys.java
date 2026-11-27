package kmod.installer;

import java.io.*;
import java.math.BigInteger;
import java.security.*;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.PKCS8EncodedKeySpec;
import java.text.SimpleDateFormat;
import java.util.*;

/** The signing key of the injected apps: generated once, kept in the app's private storage and backed up to the media folder. */
final class Keys {
    final PrivateKey key;
    final X509Certificate cert;
    Keys(PrivateKey k, X509Certificate c) { key = k; cert = c; }

    static Keys loadOrCreate(File dir, File backupDir) throws Exception {
        File pk = new File(dir, "signing.pk8"), ce = new File(dir, "signing.crt");
        if (!pk.exists() || !ce.exists()) {                                   // restore from the backup, if the app's data was cleared
            File bpk = new File(backupDir, "signing.pk8"), bce = new File(backupDir, "signing.crt");
            if (bpk.exists() && bce.exists()) { dir.mkdirs(); copy(bpk, pk); copy(bce, ce); }   // the private folder may not exist yet (fresh install)
        }
        if (pk.exists() && ce.exists()) {
            PrivateKey k = KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(read(pk)));
            X509Certificate c = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(new ByteArrayInputStream(read(ce)));
            return new Keys(k, c);
        }
        KeyPairGenerator g = KeyPairGenerator.getInstance("RSA");
        g.initialize(2048);
        KeyPair kp = g.generateKeyPair();
        byte[] der = selfSigned(kp);
        X509Certificate c = (X509Certificate) CertificateFactory.getInstance("X.509").generateCertificate(new ByteArrayInputStream(der));
        dir.mkdirs();
        write(pk, kp.getPrivate().getEncoded()); write(ce, der);
        if (backupDir != null) { backupDir.mkdirs(); write(new File(backupDir, "signing.pk8"), kp.getPrivate().getEncoded()); write(new File(backupDir, "signing.crt"), der); }
        return new Keys(kp.getPrivate(), c);
    }

    // ---- a minimal self-signed X.509 v3 certificate (DER), sha256WithRSA
    private static byte[] tlv(int tag, byte[]... parts) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        for (byte[] p : parts) body.write(p, 0, p.length);
        byte[] b = body.toByteArray();
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(tag);
        if (b.length < 128) o.write(b.length);
        else if (b.length < 256) { o.write(0x81); o.write(b.length); }
        else if (b.length < 65536) { o.write(0x82); o.write(b.length >> 8); o.write(b.length); }
        else { o.write(0x83); o.write(b.length >> 16); o.write(b.length >> 8); o.write(b.length); }
        o.write(b, 0, b.length);
        return o.toByteArray();
    }

    private static byte[] oid(int... v) {
        ByteArrayOutputStream o = new ByteArrayOutputStream();
        o.write(v[0] * 40 + v[1]);
        for (int i = 2; i < v.length; i++) {
            int x = v[i];
            if (x < 128) o.write(x);
            else {
                byte[] t = new byte[5]; int n = 0;
                while (x > 0) { t[n++] = (byte) (x & 0x7F); x >>= 7; }
                for (int k = n - 1; k >= 0; k--) o.write(t[k] | (k > 0 ? 0x80 : 0));
            }
        }
        return tlv(0x06, o.toByteArray());
    }

    private static byte[] time(Date d) {
        Calendar c = Calendar.getInstance(TimeZone.getTimeZone("UTC")); c.setTime(d);
        if (c.get(Calendar.YEAR) >= 2050) { SimpleDateFormat f = new SimpleDateFormat("yyyyMMddHHmmss'Z'", Locale.US); f.setTimeZone(TimeZone.getTimeZone("UTC")); return tlv(0x18, f.format(d).getBytes()); }
        SimpleDateFormat f = new SimpleDateFormat("yyMMddHHmmss'Z'", Locale.US); f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return tlv(0x17, f.format(d).getBytes());
    }

    private static byte[] selfSigned(KeyPair kp) throws Exception {
        byte[] sigAlg = tlv(0x30, oid(1, 2, 840, 113549, 1, 1, 11), tlv(0x05));
        byte[] name = tlv(0x30, tlv(0x31, tlv(0x30, oid(2, 5, 4, 3), tlv(0x0C, "kmod installer".getBytes("UTF-8")))));
        Calendar from = Calendar.getInstance(TimeZone.getTimeZone("UTC")); from.add(Calendar.DAY_OF_YEAR, -1);
        Calendar to = Calendar.getInstance(TimeZone.getTimeZone("UTC")); to.add(Calendar.YEAR, 50);
        byte[] serial = tlv(0x02, new BigInteger(64, new SecureRandom()).add(BigInteger.ONE).toByteArray());
        byte[] tbs = tlv(0x30, tlv(0xA0, tlv(0x02, new byte[]{2})), serial, sigAlg, name,
                tlv(0x30, time(from.getTime()), time(to.getTime())), name, kp.getPublic().getEncoded());
        Signature s = Signature.getInstance("SHA256withRSA");
        s.initSign(kp.getPrivate()); s.update(tbs);
        byte[] sig = s.sign();
        byte[] bits = new byte[sig.length + 1]; System.arraycopy(sig, 0, bits, 1, sig.length);
        return tlv(0x30, tbs, sigAlg, tlv(0x03, bits));
    }

    private static byte[] read(File f) throws IOException {
        try (FileInputStream in = new FileInputStream(f)) { ByteArrayOutputStream o = new ByteArrayOutputStream(); byte[] b = new byte[8192]; int n; while ((n = in.read(b)) > 0) o.write(b, 0, n); return o.toByteArray(); }
    }
    private static void write(File f, byte[] d) throws IOException { try (FileOutputStream o = new FileOutputStream(f)) { o.write(d); } }
    private static void copy(File a, File b) throws IOException { write(b, read(a)); }
}
