import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.file.*;
import java.util.*;

/**
 * Lab 3 data generator. Deterministic by seed.
 *   java tools/Gen.java [--seed 2026] [--out ../data]   (or ./gen.sh) [--per-day 1200000] [--alpha 1.6]
 * Writes schedule.bin, queries.txt, airlines.conf (formats: see README).
 */
public class Gen {
    static final int DAYS = 8, N_AIRPORTS = 400, N_HUBS = 8;
    static double ALPHA = 1.6;   // airport size power law (hubs are separate)
    static final String[] REAL = ("IST FRA DXB AMS CDG DOH LHR MAD "
            + "LED LIS SVO VKO BER VIE PRG WAW HEL CPH OSL ARN ZRH GVA BCN FCO MXP ATH BUD OTP SOF BEG "
            + "TLV CAI JED RUH AUH KWI BAH MCT TAS ALA TBS EVN GYD KBP MSQ RIX VNO TLL DUB BRU LYS NCE "
            + "MRS HAM DUS CGN STR LEJ NUE BLQ VCE NAP PMO OPO FAO AGP PMI ALC SVQ VLC BIO KRK GDN WRO KTW").split(" ");
    static final String[] AIRLINES = ("TK LH EK KL AF QR BA IB "
            + "SU TP U6 S7 FZ PC W6 FR U2 LX OS SK AY LO OK BT A3 RO FB JU MS RJ ME PS HY KC J2 VY UX AZ G9 DY").split(" ");

    public static void main(String[] args) throws IOException {
        long seed = 2026;
        Path out = Path.of(".");
        int perDay = 1_200_000;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--seed" -> seed = Long.parseLong(args[++i]);
                case "--out" -> out = Path.of(args[++i]);
                case "--per-day" -> perDay = Integer.parseInt(args[++i]);
                case "--alpha" -> ALPHA = Double.parseDouble(args[++i]);
                default -> throw new IllegalArgumentException("unknown option " + args[i]);
            }
        }
        Files.createDirectories(out);
        SplittableRandom rnd = new SplittableRandom(seed);

        // Airports: rank 0..7 are hubs, then decreasing size. Codes: real list, then random unique.
        String[] codes = new String[N_AIRPORTS];
        Set<String> used = new HashSet<>();
        for (int i = 0; i < N_AIRPORTS; i++) {
            String c;
            if (i < REAL.length) c = REAL[i];
            else do {
                c = "" + (char) ('A' + rnd.nextInt(26)) + (char) ('A' + rnd.nextInt(26)) + (char) ('A' + rnd.nextInt(26));
            } while (used.contains(c));
            used.add(c);
            codes[i] = c;
        }
        double[] x = new double[N_AIRPORTS], y = new double[N_AIRPORTS];
        for (int i = 0; i < N_AIRPORTS; i++) { x[i] = rnd.nextDouble() * 6000; y[i] = rnd.nextDouble() * 4000; }

        double[] weight = new double[N_AIRPORTS];
        for (int i = 0; i < N_AIRPORTS; i++)
            weight[i] = i < N_HUBS ? 0 : 1.0 / Math.pow(i - N_HUBS + 3, ALPHA);
        double[] cum = cumulative(weight);

        int nAl = AIRLINES.length;
        double[] alFactor = new double[nAl];
        for (int a = 0; a < nAl; a++) alFactor[a] = 0.75 + 0.6 * rnd.nextDouble();

        // One day template.
        int[] tFrom = new int[perDay], tTo = new int[perDay], tDep = new int[perDay], tDur = new int[perDay],
                tAl = new int[perDay], tNo = new int[perDay], tBase = new int[perDay];
        int[] nextNo = new int[nAl];
        for (int a = 0; a < nAl; a++) nextNo[a] = 100 + rnd.nextInt(200);
        for (int i = 0; i < perDay; i++) {
            int a, b;
            if (rnd.nextDouble() < 0.7) {
                a = rnd.nextInt(N_HUBS);
                b = rnd.nextDouble() < 0.25 ? rnd.nextInt(N_HUBS) : pick(cum, rnd);
                while (b == a) b = pick(cum, rnd);
                if (rnd.nextBoolean()) { int t = a; a = b; b = t; }
            } else {
                a = pick(cum, rnd);
                do b = pick(cum, rnd); while (b == a);
            }
            double dist = Math.hypot(x[a] - x[b], y[a] - y[b]);
            int al = pairAirline(a, b, seed, rnd.nextInt(3));
            tFrom[i] = a; tTo[i] = b;
            tDep[i] = rnd.nextInt(1440);
            tDur[i] = 45 + (int) (dist / 750 * 60);
            tAl[i] = al;
            tNo[i] = nextNo[al]++ & 0xFFFF;
            tBase[i] = (int) ((1500 + dist * 3.5) * alFactor[al]);
        }

        int n = perDay * DAYS;
        int[] order = new int[n];
        for (int i = 0; i < n; i++) order[i] = i;
        for (int i = n - 1; i > 0; i--) { int j = rnd.nextInt(i + 1); int t = order[i]; order[i] = order[j]; order[j] = t; }

        Path sched = out.resolve("schedule.bin");
        try (FileChannel ch = FileChannel.open(sched, StandardOpenOption.CREATE, StandardOpenOption.WRITE,
                StandardOpenOption.TRUNCATE_EXISTING)) {
            ByteBuffer head = ByteBuffer.allocate(16 + N_AIRPORTS * 3 + nAl * 2).order(ByteOrder.LITTLE_ENDIAN);
            head.put("FLT1".getBytes()).putInt(N_AIRPORTS).putInt(nAl).putInt(n);
            for (String c : codes) head.put(c.getBytes());
            for (String c : AIRLINES) head.put(c.getBytes());
            head.flip();
            ch.write(head);
            ByteBuffer buf = ByteBuffer.allocate(20 * 65536).order(ByteOrder.LITTLE_ENDIAN);
            for (int k = 0; k < n; k++) {
                int src = order[k], day = src / perDay, i = src % perDay;
                int dep = day * 1440 + tDep[i];
                // the same flight costs a bit differently on different days
                double dayFactor = 0.85 + 0.4 * unit(seed, src);
                int price = (int) Math.round(tBase[i] * dayFactor / 10.0) * 10;
                buf.putInt(dep).putInt(dep + tDur[i]).putShort((short) tFrom[i]).putShort((short) tTo[i])
                        .putShort((short) tAl[i]).putShort((short) tNo[i]).putInt(price);
                if (!buf.hasRemaining()) { buf.flip(); ch.write(buf); buf.clear(); }
            }
            buf.flip();
            ch.write(buf);
        }

        // Queries: heavy = between large non-hub airports (routes go through hubs), light = small airports.
        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("queries.txt")))) {
            Set<String> seen = new HashSet<>();
            // heavy: a small airport to a hub or a large airport -> few first flights, each opening a huge subtree
            writeQueries(w, codes, rnd, 6, 330, N_AIRPORTS, 0, N_HUBS, seen);
            writeQueries(w, codes, rnd, 4, 330, N_AIRPORTS, N_HUBS, N_HUBS + 12, seen);
            // light: small to small
            writeQueries(w, codes, rnd, 10, 250, N_AIRPORTS, 250, N_AIRPORTS, seen);
        }

        try (PrintWriter w = new PrintWriter(Files.newBufferedWriter(out.resolve("airlines.conf")))) {
            w.println("# AL limit medianMs sigma tailProb tailMs errProb");
            int tailA = N_HUBS + 3 + rnd.nextInt(10), tailB = N_HUBS + 15 + rnd.nextInt(10);
            for (int a = 0; a < nAl; a++) {
                int limit = a < N_HUBS ? 150 + 10 * rnd.nextInt(6) : 20 + 10 * rnd.nextInt(10);
                int median = 30 + 5 * rnd.nextInt(19);
                double sigma = 0.3 + 0.05 * rnd.nextInt(7);
                double tailProb = a == tailA ? 0.08 : a == tailB ? 0.04 : 0.0;
                int tailMs = a == tailA ? 3000 : a == tailB ? 2000 : 0;
                double err = rnd.nextDouble() < 0.4 ? 0.01 * (1 + rnd.nextInt(5)) : 0.0;
                w.printf(Locale.ROOT, "%s %d %d %.2f %.2f %d %.2f%n", AIRLINES[a], limit, median, sigma, tailProb, tailMs, err);
            }
        }
        System.out.printf("flights=%d airports=%d airlines=%d file=%.1f MB%n", n, N_AIRPORTS, nAl, Files.size(sched) / 1e6);
    }

    static void writeQueries(PrintWriter w, String[] codes, SplittableRandom rnd, int count,
                             int fromLo, int fromHi, int toLo, int toHi, Set<String> seen) {
        for (int k = 0; k < count; ) {
            int a = fromLo + rnd.nextInt(fromHi - fromLo), b = toLo + rnd.nextInt(toHi - toLo);
            if (a == b || !seen.add(a + ":" + b)) continue;
            w.println(codes[a] + " " + codes[b] + " " + rnd.nextInt(7));
            k++;
        }
    }

    /** 1–3 airlines per airport pair; a hub's home carrier always flies from/to its hub. */
    static int pairAirline(int a, int b, long seed, int slot) {
        int lo = Math.min(a, b), hi = Math.max(a, b);
        long h = mix(seed ^ ((long) lo << 20 | hi));
        int served = 1 + (int) Long.remainderUnsigned(h, 3);
        slot %= served;
        int hub = a < N_HUBS ? a : b < N_HUBS ? b : -1;
        if (slot == 0 && hub >= 0) return hub;
        return N_HUBS + (int) Long.remainderUnsigned(mix(h + slot), AIRLINES.length - N_HUBS);
    }

    static int pick(double[] cum, SplittableRandom rnd) {
        double r = rnd.nextDouble() * cum[cum.length - 1];
        int i = Arrays.binarySearch(cum, r);
        return i >= 0 ? i : -i - 1;
    }

    static double[] cumulative(double[] w) {
        double[] c = new double[w.length];
        double s = 0;
        for (int i = 0; i < w.length; i++) c[i] = s += w[i];
        return c;
    }

    static double unit(long seed, long k) { return (mix(seed * 31 + k) >>> 11) * 0x1.0p-53; }

    static long mix(long z) {
        z += 0x9E3779B97F4A7C15L;
        z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
        z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
        return z ^ (z >>> 31);
    }
}
