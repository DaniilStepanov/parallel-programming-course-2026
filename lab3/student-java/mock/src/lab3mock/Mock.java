package lab3mock;

import lab3.Pricing;
import lab3.Schedule;

import java.io.*;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Airline mock for lab 3, stage 2. One process plays all airlines.
 *
 *   java -cp out lab3mock.Mock --schedule ../data/schedule.bin --airlines ../data/airlines.conf [--port 7700]
 *
 * Protocol (text lines over TCP; reqId is unique within a connection):
 *   -> PRICE reqId searchId AL FROM TO D      <- reqId OK [flightId:price ...] | reqId ERR | reqId LIMIT
 *   -> CANCEL reqId                            (no answer)
 *   -> DONE searchId                           <- DONE searchId orphans [AL:FROM:TO:D@ageMs ...]
 *   -> STATS                                   <- {json}
 *   -> RESET                                   <- RESET
 */
public final class Mock {
    static final long DONE_GRACE_MS = 20;

    record Airline(String code, int limit, double medianMs, double sigma, double tailProb, int tailMs, double errProb) {}

    final Schedule sch;
    final Airline[] airlines;
    final AtomicInteger[] inFlight;
    final AtomicInteger[] maxInFlight;
    /** sorted (segmentKey << 24 | flightId) */
    final long[] index;
    final ScheduledThreadPoolExecutor timer;
    final ConcurrentHashMap<Long, SearchState> searches = new ConcurrentHashMap<>();
    final AtomicLong total = new AtomicLong(), ok = new AtomicLong(), err = new AtomicLong(), limit = new AtomicLong(),
            cancelled = new AtomicLong(), orphans = new AtomicLong(), dones = new AtomicLong(), bad = new AtomicLong();

    Mock(Schedule sch, Airline[] byId) {
        this.sch = sch;
        this.airlines = byId;
        inFlight = new AtomicInteger[byId.length];
        maxInFlight = new AtomicInteger[byId.length];
        for (int i = 0; i < byId.length; i++) { inFlight[i] = new AtomicInteger(); maxInFlight[i] = new AtomicInteger(); }
        index = buildIndex(sch);
        timer = new ScheduledThreadPoolExecutor(Math.max(2, Runtime.getRuntime().availableProcessors() / 4), r -> {
            Thread t = new Thread(r, "mock-timer");
            t.setDaemon(true);
            return t;
        });
        timer.setRemoveOnCancelPolicy(true);
    }

    long segKey(int al, int from, int to, int day) {
        return (((long) al * sch.nAirports + from) * sch.nAirports + to) * 16 + day;
    }

    long[] buildIndex(Schedule s) {
        long[] a = new long[s.nFlights];
        for (int f = 0; f < s.nFlights; f++)
            a[f] = segKey(s.airline(f), s.from(f), s.to(f), s.dep(f) / 1440) << 24 | f;
        Arrays.parallelSort(a);
        return a;
    }

    /** Flight ids of airline al on from->to departing on day. */
    int[] flights(int al, int from, int to, int day) {
        long k = segKey(al, from, to, day);
        int lo = lowerBound(k << 24), hi = lowerBound((k + 1) << 24);
        int[] r = new int[hi - lo];
        for (int i = lo; i < hi; i++) r[i - lo] = (int) (index[i] & 0xFFFFFF);
        return r;
    }

    int lowerBound(long v) {
        int lo = 0, hi = index.length;
        while (lo < hi) { int m = (lo + hi) >>> 1; if (index[m] < v) lo = m + 1; else hi = m; }
        return lo;
    }

    // ---------------------------------------------------------------- requests and searches

    static final class Pending {
        final long reqId, searchId;
        final int al;
        final String key;
        final Conn conn;
        final AtomicInteger state = new AtomicInteger(); // 0 in flight, 1 answered, 2 cancelled
        volatile ScheduledFuture<?> task;
        Pending(long reqId, long searchId, int al, String key, Conn conn) {
            this.reqId = reqId; this.searchId = searchId; this.al = al; this.key = key; this.conn = conn;
        }
    }

    static final class SearchState {
        final List<Pending> requests = new ArrayList<>();
        final Map<String, Long> okAtNanos = new HashMap<>();
    }

    SearchState search(long id) { return searches.computeIfAbsent(id, k -> new SearchState()); }

    void price(Conn c, String[] p) {
        if (p.length != 7) { bad.incrementAndGet(); return; }
        long reqId = Long.parseLong(p[1]), searchId = Long.parseLong(p[2]);
        int al = sch.airlineId(p[3]), from = sch.airportId(p[4]), to = sch.airportId(p[5]), day = Integer.parseInt(p[6]);
        total.incrementAndGet();
        if (al < 0 || from < 0 || to < 0 || day < 0 || day > 15) { bad.incrementAndGet(); c.send(reqId + " ERR"); return; }
        Airline a = airlines[al];
        int now = inFlight[al].incrementAndGet();
        if (now > a.limit) {
            inFlight[al].decrementAndGet();
            limit.incrementAndGet();
            c.send(reqId + " LIMIT");
            return;
        }
        maxInFlight[al].accumulateAndGet(now, Math::max);
        String key = p[3] + ":" + p[4] + ":" + p[5] + ":" + day;
        Pending r = new Pending(reqId, searchId, al, key, c);
        SearchState s = search(searchId);
        synchronized (s) { s.requests.add(r); }
        c.pending.put(reqId, r);
        ThreadLocalRandom rnd = ThreadLocalRandom.current();
        boolean error = rnd.nextDouble() < a.errProb;
        double ms;
        if (error) ms = 5 + rnd.nextDouble() * 10;
        else if (rnd.nextDouble() < a.tailProb) ms = a.tailMs;
        else ms = a.medianMs * Math.exp(a.sigma * rnd.nextGaussian());
        r.task = timer.schedule(() -> answer(r, error, from, to, day), (long) (ms * 1000), TimeUnit.MICROSECONDS);
    }

    void answer(Pending r, boolean error, int from, int to, int day) {
        if (!r.state.compareAndSet(0, 1)) return;
        r.conn.pending.remove(r.reqId);
        inFlight[r.al].decrementAndGet();
        if (error) {
            err.incrementAndGet();
            r.conn.send(r.reqId + " ERR");
            return;
        }
        StringBuilder sb = new StringBuilder(64).append(r.reqId).append(" OK");
        for (int f : flights(r.al, from, to, day)) {
            if (Pricing.noSeats(f, r.searchId)) continue;
            sb.append(' ').append(f).append(':').append(Pricing.price(f, r.searchId, sch.price(f)));
        }
        SearchState s = searches.get(r.searchId);
        if (s != null) synchronized (s) { s.okAtNanos.putIfAbsent(r.key, System.nanoTime()); }
        ok.incrementAndGet();
        r.conn.send(sb.toString());
    }

    void cancel(Conn c, long reqId) {
        Pending r = c.pending.remove(reqId);
        if (r == null || !r.state.compareAndSet(0, 2)) return;
        ScheduledFuture<?> t = r.task;
        if (t != null) t.cancel(false);
        inFlight[r.al].decrementAndGet();
        cancelled.incrementAndGet();
    }

    void done(Conn c, long searchId) {
        long at = System.nanoTime();
        SearchState s = searches.remove(searchId);
        dones.incrementAndGet();
        if (s == null) { c.send("DONE " + searchId + " 0"); return; }
        StringBuilder keys = new StringBuilder();
        List<Pending> open = new ArrayList<>();
        synchronized (s) {
            for (Map.Entry<String, Long> e : s.okAtNanos.entrySet())
                keys.append(' ').append(e.getKey()).append('@').append((at - e.getValue()) / 1_000_000);
            for (Pending r : s.requests) if (r.state.get() == 0) open.add(r);
        }
        // CANCELs travel on other connections than DONE: give them a moment to arrive.
        timer.schedule(() -> {
            int n = 0;
            for (Pending r : open) if (r.state.get() == 0) n++;
            orphans.addAndGet(n);
            c.send("DONE " + searchId + " " + n + keys);
        }, DONE_GRACE_MS, TimeUnit.MILLISECONDS);
    }

    String stats() {
        StringBuilder sb = new StringBuilder("{");
        sb.append("\"requests\":").append(total.get()).append(",\"ok\":").append(ok.get()).append(",\"err\":").append(err.get())
          .append(",\"limit\":").append(limit.get()).append(",\"cancelled\":").append(cancelled.get())
          .append(",\"orphans\":").append(orphans.get()).append(",\"searchesDone\":").append(dones.get())
          .append(",\"badLines\":").append(bad.get());
        int in = 0;
        for (AtomicInteger x : inFlight) in += x.get();
        sb.append(",\"inFlightNow\":").append(in).append(",\"maxInFlight\":{");
        boolean first = true;
        for (int i = 0; i < airlines.length; i++) {
            if (airlines[i] == null) continue;
            if (!first) sb.append(',');
            first = false;
            sb.append('"').append(airlines[i].code).append("\":").append(maxInFlight[i].get());
        }
        return sb.append("}}").toString();
    }

    void reset() {
        for (AtomicLong x : List.of(total, ok, err, limit, cancelled, orphans, dones, bad)) x.set(0);
        for (int i = 0; i < airlines.length; i++) maxInFlight[i].set(inFlight[i].get());
        searches.clear();
    }

    // ---------------------------------------------------------------- connections

    final class Conn {
        final Socket sock;
        final ConcurrentHashMap<Long, Pending> pending = new ConcurrentHashMap<>();
        final LinkedBlockingQueue<String> out = new LinkedBlockingQueue<>();

        Conn(Socket sock) { this.sock = sock; }

        void send(String line) { out.add(line); }

        void writeLoop() {
            try (Writer w = new BufferedWriter(new OutputStreamWriter(sock.getOutputStream(), StandardCharsets.US_ASCII), 1 << 16)) {
                List<String> batch = new ArrayList<>();
                while (true) {
                    batch.add(out.take());
                    out.drainTo(batch);
                    for (String s : batch) { w.write(s); w.write('\n'); }
                    w.flush();
                    batch.clear();
                }
            } catch (IOException | InterruptedException ignored) {
            }
        }

        void readLoop() {
            Thread writer = Thread.ofVirtual().start(this::writeLoop);
            try (BufferedReader in = new BufferedReader(new InputStreamReader(sock.getInputStream(), StandardCharsets.US_ASCII), 1 << 16)) {
                String line;
                while ((line = in.readLine()) != null) {
                    String[] p = line.split(" ");
                    try {
                        switch (p[0]) {
                            case "PRICE" -> price(this, p);
                            case "CANCEL" -> cancel(this, Long.parseLong(p[1]));
                            case "DONE" -> done(this, Long.parseLong(p[1]));
                            case "STATS" -> send(stats());
                            case "RESET" -> { reset(); send("RESET"); }
                            default -> bad.incrementAndGet();
                        }
                    } catch (RuntimeException e) {
                        bad.incrementAndGet();
                    }
                }
            } catch (IOException ignored) {
            } finally {
                for (Pending r : pending.values()) {
                    if (r.state.compareAndSet(0, 2)) {
                        ScheduledFuture<?> t = r.task;
                        if (t != null) t.cancel(false);
                        inFlight[r.al].decrementAndGet();
                    }
                }
                writer.interrupt();
                try { sock.close(); } catch (IOException ignored) {}
            }
        }
    }

    static Airline[] loadAirlines(Path conf, Schedule s) throws IOException {
        Airline[] byId = new Airline[s.nAirlines];
        for (String line : Files.readAllLines(conf)) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            int id = s.airlineId(p[0]);
            if (id < 0) throw new IOException("unknown airline in " + conf + ": " + p[0]);
            byId[id] = new Airline(p[0], Integer.parseInt(p[1]), Double.parseDouble(p[2]), Double.parseDouble(p[3]),
                    Double.parseDouble(p[4]), Integer.parseInt(p[5]), Double.parseDouble(p[6]));
        }
        for (int i = 0; i < byId.length; i++)
            if (byId[i] == null) throw new IOException("airline " + s.airlineCode(i) + " missing in " + conf);
        return byId;
    }

    public static void main(String[] args) throws Exception {
        Map<String, String> a = new HashMap<>(Map.of("--schedule", "../data/schedule.bin", "--airlines", "../data/airlines.conf", "--port", "7700"));
        for (int i = 0; i + 1 < args.length; i += 2) a.put(args[i], args[i + 1]);
        long t0 = System.nanoTime();
        Schedule s = Schedule.open(Path.of(a.get("--schedule")));
        Mock m = new Mock(s, loadAirlines(Path.of(a.get("--airlines")), s));
        int port = Integer.parseInt(a.get("--port"));
        try (ServerSocket ss = new ServerSocket()) {
            ss.setReuseAddress(true);
            ss.bind(new InetSocketAddress("127.0.0.1", port), 1024);
            System.out.printf("mock: %d flights, %d airlines, indexed in %d ms, listening on 127.0.0.1:%d%n",
                    s.nFlights, s.nAirlines, (System.nanoTime() - t0) / 1_000_000, port);
            while (true) {
                Socket c = ss.accept();
                c.setTcpNoDelay(true);
                Thread.ofVirtual().start(m.new Conn(c)::readLoop);
            }
        }
    }
}
