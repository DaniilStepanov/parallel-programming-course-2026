package lab3;

import java.io.*;
import java.lang.management.ManagementFactory;
import java.lang.reflect.Constructor;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.LockSupport;

/**
 * Stage 2 harness: starts searches at a fixed rate (open loop), measures latency from the planned start,
 * checks every answer against the airline mock and prints the metrics.
 *
 *   java -cp out lab3.Harness --impl lab3.StudentAggregator --rate 200 [--seconds 20] [--warmup 5]
 *        [--sweep 50,100,200] [--data ../data] [--host 127.0.0.1 --port 7700] [--connections 4]
 *        [--opt key=value ...]
 */
public final class Harness {
    static final long DEADLINE_NS = 300_000_000L;
    /** An OK answer the mock sent less than this before it got DONE may not have reached the aggregator in time. */
    static final long IN_TRANSIT_MS = 20;

    record Template(String from, String to, int day, List<Search.Route> routes) {}

    final Map<String, String> args;
    final Schedule schedule;
    final List<Template> templates;
    final AirlineClient client;
    final Control control;
    final Map<String, Integer> limits;
    final Map<String, String> options = new HashMap<>();
    final AtomicLong nextSearchId = new AtomicLong(1);

    Harness(Map<String, String> args, List<String> opts) throws IOException {
        this.args = args;
        Path data = Path.of(args.get("--data"));
        schedule = Schedule.open(data.resolve("schedule.bin"));
        templates = loadSearches(data.resolve("searches.txt"), schedule);
        limits = Kit.loadLimits(data.resolve("airlines.conf"));
        for (String o : opts) {
            int eq = o.indexOf('=');
            if (eq < 0) throw new IllegalArgumentException("--opt expects key=value: " + o);
            options.put(o.substring(0, eq), o.substring(eq + 1));
        }
        String host = args.get("--host");
        int port = Integer.parseInt(args.get("--port"));
        control = new Control(host, port);
        control.reset();
        client = new AirlineClient(host, port, Integer.parseInt(args.get("--connections")));
    }

    static List<Template> loadSearches(Path file, Schedule s) throws IOException {
        List<Template> list = new ArrayList<>();
        for (String line : Files.readAllLines(file)) {
            if (line.isBlank()) continue;
            int bar = line.indexOf('|');
            String[] head = line.substring(0, bar).trim().split("\\s+");
            List<Search.Route> routes = new ArrayList<>();
            for (String r : line.substring(bar + 1).trim().split("\\s+")) {
                List<Flight> fl = new ArrayList<>(3);
                for (String id : r.split("-")) fl.add(s.flight(Integer.parseInt(id)));
                routes.add(new Search.Route(List.copyOf(fl)));
            }
            list.add(new Template(head[1], head[2], Integer.parseInt(head[3]), List.copyOf(routes)));
        }
        return list;
    }

    // ------------------------------------------------------------------ one run at a fixed rate

    record Result(double rate, int measured, double p50, double p99, double max, int late, int partial, double answered, int wrong,
                  long orphans, long limit, double requestsPerSearch, int platformThreads, long rssMb, List<String> errors) {}

    final class Run {
        final double rate;
        final long warmupNs, totalNs;
        final ConcurrentLinkedQueue<Long> latenciesNs = new ConcurrentLinkedQueue<>();
        final AtomicInteger late = new AtomicInteger(), partial = new AtomicInteger(), wrong = new AtomicInteger(),
                measured = new AtomicInteger();
        final AtomicLong orphans = new AtomicLong(), keys = new AtomicLong(), keysAnswered = new AtomicLong();
        final ConcurrentLinkedQueue<String> errors = new ConcurrentLinkedQueue<>();
        final Phaser outstanding = new Phaser(1);
        volatile int peakThreads;
        volatile long peakRss;

        Run(double rate, double seconds, double warmup) {
            this.rate = rate;
            warmupNs = (long) (warmup * 1e9);
            totalNs = (long) ((warmup + seconds) * 1e9);
        }

        Result go(Aggregator agg) throws Exception {
            ScheduledExecutorService sampler = Executors.newSingleThreadScheduledExecutor(r -> {
                Thread t = new Thread(r, "harness-sampler");
                t.setDaemon(true);
                return t;
            });
            sampler.scheduleAtFixedRate(this::sample, 0, 200, TimeUnit.MILLISECONDS);
            long t0 = System.nanoTime(), periodNs = (long) (1e9 / rate);
            String statsAtWarmup = null;
            for (long k = 0; ; k++) {
                long planned = t0 + k * periodNs;
                if (planned - t0 >= totalNs) break;
                if (statsAtWarmup == null && planned - t0 >= warmupNs) statsAtWarmup = control.stats();
                long wait = planned - System.nanoTime();
                if (wait > 0) LockSupport.parkNanos(wait);
                start(agg, planned, planned - t0 >= warmupNs);
            }
            String statsAtEnd = control.stats();
            outstanding.arriveAndDeregister();
            try {
                outstanding.awaitAdvanceInterruptibly(0, 10, TimeUnit.SECONDS);
            } catch (TimeoutException e) {
                errors.add("some searches never completed (10 s after the last start)");
            }
            sampler.shutdownNow();
            int n = measured.get();
            long[] lat = latenciesNs.stream().mapToLong(Long::longValue).sorted().toArray();
            long reqs = jsonLong(statsAtEnd, "requests") - jsonLong(statsAtWarmup, "requests");
            long lim = jsonLong(control.stats(), "limit") - jsonLong(statsAtWarmup, "limit");
            int unanswered = n - lat.length;
            return new Result(rate, n, pct(lat, 0.50), pct(lat, 0.99), lat.length == 0 ? 0 : lat[lat.length - 1] / 1e6,
                    late.get() + unanswered, partial.get(), 100.0 * keysAnswered.get() / Math.max(1, keys.get()), wrong.get() + unanswered, orphans.get(), lim,
                    n == 0 ? 0 : (double) reqs / Math.max(1, (long) (rate * (totalNs - warmupNs) / 1e9)),
                    peakThreads, peakRss, List.copyOf(errors));
        }

        void sample() {
            peakThreads = Math.max(peakThreads, ManagementFactory.getThreadMXBean().getThreadCount());
            peakRss = Math.max(peakRss, rssMb());
        }

        void start(Aggregator agg, long planned, boolean counted) {
            Template t = templates.get((int) Math.floorMod(nextSearchId.get(), (long) templates.size()));
            Search s = new Search(nextSearchId.getAndIncrement(), t.from, t.to, t.day, t.routes, planned + DEADLINE_NS);
            if (counted) measured.incrementAndGet();
            outstanding.register();
            CompletableFuture<Answer> f;
            try {
                f = agg.search(s);
            } catch (Throwable e) {
                f = CompletableFuture.failedFuture(e);
            }
            f.whenComplete((a, e) -> finished(s, planned, counted, a, e));
        }

        void finished(Search s, long planned, boolean counted, Answer a, Throwable e) {
            long doneAt = System.nanoTime();
            long lat = doneAt - planned;
            control.done(s.id()).whenComplete((d, ex) -> {
                try {
                    if (!counted) return;
                    latenciesNs.add(lat);
                    if (lat > DEADLINE_NS) late.incrementAndGet();
                    if (ex != null) { error(s, "no DONE answer from mock: " + ex); return; }
                    orphans.addAndGet(d.orphans);
                    if (e != null) { error(s, "search failed: " + e); return; }
                    if (a.partial()) partial.incrementAndGet();
                    Set<String> uniq = new HashSet<>();
                    for (Search.Route r : s.routes()) for (Flight f : r.flights()) uniq.add(f.priceKey().toString());
                    keys.addAndGet(uniq.size());
                    for (String k : uniq) if (d.answeredAgeMs.containsKey(k)) keysAnswered.incrementAndGet();
                    String why = check(s, a, d);
                    if (why != null) error(s, why);
                } finally {
                    outstanding.arriveAndDeregister();
                }
            });
        }

        void error(Search s, String why) {
            wrong.incrementAndGet();
            if (errors.size() < 10) errors.add("search " + s.id() + ": " + why);
        }
    }

    /** @return null if the answer is right, otherwise what is wrong */
    static String check(Search s, Answer a, Control.Done d) {
        if (a == null || a.routes() == null) return "null answer";
        if (a.routes().size() != s.routes().size()) return "answer has " + a.routes().size() + " routes, search has " + s.routes().size();
        for (int i = 0; i < s.routes().size(); i++) {
            List<Flight> fl = s.routes().get(i).flights();
            Answer.RouteResult r = a.routes().get(i);
            if (r == null) return "route " + i + ": null result";
            boolean allAnswered = true, settledBefore = true, anyNoSeats = false;
            int sum = 0;
            for (Flight f : fl) {
                Long age = d.answeredAgeMs.get(f.priceKey().toString());
                if (age == null) { allAnswered = false; continue; }
                if (age < IN_TRANSIT_MS) settledBefore = false;
                if (Pricing.noSeats(f.id(), s.id())) anyNoSeats = true;
                else sum += Pricing.price(f.id(), s.id(), f.basePrice());
            }
            boolean noSeatsSeen = false;
            for (Flight f : fl)
                if (d.answeredAgeMs.containsKey(f.priceKey().toString()) && Pricing.noSeats(f.id(), s.id())) noSeatsSeen = true;
            switch (r.status()) {
                case PRICED -> {
                    if (!allAnswered) return "route " + i + ": PRICED, but the mock did not answer OK for every flight";
                    if (anyNoSeats) return "route " + i + ": PRICED, but a flight has no seats";
                    if (r.price() != sum) return "route " + i + ": price " + r.price() + ", expected " + sum;
                }
                case NO_SEATS -> {
                    if (!noSeatsSeen) return "route " + i + ": NO_SEATS, but no answered flight of the route lacks seats";
                }
                case UNPRICED -> {
                    if (allAnswered && settledBefore && !anyNoSeats)
                        return "route " + i + ": UNPRICED, but the mock had answered OK for every flight before the answer";
                    if (allAnswered && settledBefore && anyNoSeats)
                        return "route " + i + ": UNPRICED, but the answers show NO_SEATS";
                }
            }
        }
        Answer expected = Answer.of(s, a.routes());
        if (!expected.top().equals(a.top())) return "top is " + a.top() + ", expected " + expected.top();
        return null;
    }

    static double pct(long[] sorted, double p) {
        if (sorted.length == 0) return 0;
        return sorted[Math.min(sorted.length - 1, (int) Math.ceil(p * sorted.length) - 1)] / 1e6;
    }

    static long jsonLong(String json, String key) {
        int i = json.indexOf("\"" + key + "\":");
        if (i < 0) return 0;
        i += key.length() + 3;
        int j = i;
        while (j < json.length() && Character.isDigit(json.charAt(j))) j++;
        return Long.parseLong(json, i, j, 10);
    }

    static long rssMb() {
        try {
            for (String l : Files.readAllLines(Path.of("/proc/self/status")))
                if (l.startsWith("VmRSS:")) return Long.parseLong(l.replaceAll("\\D", "")) / 1024;
        } catch (IOException | RuntimeException ignored) {
        }
        return -1;
    }

    Aggregator newAggregator(String impl) throws Exception {
        Constructor<?> c = Class.forName(impl).getConstructor(Kit.class);
        return (Aggregator) c.newInstance(new Kit(client, schedule, limits, Map.copyOf(options)));
    }

    // ------------------------------------------------------------------ control connection (DONE / STATS / RESET)

    static final class Control {
        record Done(long orphans, Map<String, Long> answeredAgeMs) {}

        final Socket sock;
        final Writer out;
        final ConcurrentHashMap<Long, CompletableFuture<Done>> dones = new ConcurrentHashMap<>();
        final LinkedBlockingQueue<String> replies = new LinkedBlockingQueue<>();

        Control(String host, int port) throws IOException {
            sock = new Socket();
            sock.setTcpNoDelay(true);
            try {
                sock.connect(new InetSocketAddress(host, port));
            } catch (IOException e) {
                throw new IOException("cannot connect to the airline mock at " + host + ":" + port + " (is ./run-mock.sh running?)", e);
            }
            out = new BufferedWriter(new OutputStreamWriter(sock.getOutputStream(), StandardCharsets.US_ASCII));
            Thread t = new Thread(this::readLoop, "harness-control");
            t.setDaemon(true);
            t.start();
        }

        void write(String line) {
            try {
                synchronized (out) { out.write(line); out.write('\n'); out.flush(); }
            } catch (IOException e) {
                throw new UncheckedIOException(e);
            }
        }

        CompletableFuture<Done> done(long searchId) {
            CompletableFuture<Done> f = new CompletableFuture<>();
            dones.put(searchId, f);
            write("DONE " + searchId);
            return f.orTimeout(5, TimeUnit.SECONDS);
        }

        String stats() {
            write("STATS");
            return reply();
        }

        void reset() {
            write("RESET");
            reply();
        }

        String reply() {
            try {
                String r = replies.poll(5, TimeUnit.SECONDS);
                if (r == null) throw new IllegalStateException("mock did not reply");
                return r;
            } catch (InterruptedException e) {
                throw new IllegalStateException(e);
            }
        }

        void readLoop() {
            try (BufferedReader in = new BufferedReader(new InputStreamReader(sock.getInputStream(), StandardCharsets.US_ASCII), 1 << 16)) {
                String line;
                while ((line = in.readLine()) != null) {
                    if (!line.startsWith("DONE ")) { replies.add(line); continue; }
                    String[] p = line.split(" ");
                    Map<String, Long> ages = new HashMap<>();
                    for (int i = 3; i < p.length; i++) {
                        int at = p[i].indexOf('@');
                        ages.put(p[i].substring(0, at), Long.parseLong(p[i].substring(at + 1)));
                    }
                    CompletableFuture<Done> f = dones.remove(Long.parseLong(p[1]));
                    if (f != null) f.complete(new Done(Long.parseLong(p[2]), ages));
                }
            } catch (IOException ignored) {
            }
        }
    }

    // ------------------------------------------------------------------ main

    static void print(Result r, boolean header) {
        if (header) System.out.printf("%7s %7s %8s %8s %8s %6s %8s %9s %6s %8s %6s %9s %8s %7s%n",
                "rate", "n", "p50ms", "p99ms", "maxms", "late%", "partial%", "answered%", "wrong", "orphans", "LIMIT", "req/srch", "threads", "rssMB");
        System.out.printf(Locale.ROOT, "%7.0f %7d %8.1f %8.1f %8.1f %6.2f %8.2f %9.2f %6d %8d %6d %9.1f %8d %7d   %s%n",
                r.rate, r.measured, r.p50, r.p99, r.max, 100.0 * r.late / Math.max(1, r.measured),
                100.0 * r.partial / Math.max(1, r.measured), r.answered, r.wrong, r.orphans, r.limit, r.requestsPerSearch,
                r.platformThreads, r.rssMb, holds(r) ? "HOLDS" : "fails");
    }

    /**
     * p99 ≤ 300 ms, late ≤ 1 %, ≥ 98 % of airline requests answered in time, no wrong answers, no orphans, no LIMIT.
     * With a deadline the latency stays under 300 ms by construction; overload shows up as unanswered requests.
     */
    static final double MIN_ANSWERED_PCT = 98;

    static boolean holds(Result r) {
        return r.measured > 0 && r.p99 <= 300 && r.late <= 0.01 * r.measured && r.answered >= MIN_ANSWERED_PCT
                && r.wrong == 0 && r.orphans == 0 && r.limit == 0;
    }

    public static void main(String[] argv) throws Exception {
        Map<String, String> a = new HashMap<>(Map.of("--data", "../data", "--host", "127.0.0.1", "--port", "7700",
                "--seconds", "20", "--warmup", "5", "--connections", "8", "--impl", "lab3.StudentAggregator"));
        List<String> opts = new ArrayList<>();
        for (int i = 0; i < argv.length; i++) {
            String k = argv[i];
            if (!k.startsWith("--") || i + 1 >= argv.length) { System.err.println("bad argument: " + k); System.exit(2); }
            if (k.equals("--opt")) opts.add(argv[++i]);
            else a.put(k, argv[++i]);
        }
        if (!a.containsKey("--rate") && !a.containsKey("--sweep")) {
            System.err.println("usage: run.sh --impl <class> (--rate R | --sweep R1,R2,...) [--seconds 20] [--warmup 5] [--opt key=value]");
            System.exit(2);
        }
        Harness h = new Harness(a, opts);
        String impl = a.get("--impl");
        List<Double> rates = new ArrayList<>();
        for (String r : a.getOrDefault("--sweep", a.get("--rate")).split(",")) rates.add(Double.parseDouble(r));
        System.out.printf("impl %s, %d search templates, deadline 300 ms, %s s + %s s warm-up per rate, options %s%n",
                impl, h.templates.size(), a.get("--seconds"), a.get("--warmup"), h.options);
        boolean header = true;
        List<String> allErrors = new ArrayList<>();
        for (double rate : rates) {
            Aggregator agg = h.newAggregator(impl);
            Result r;
            try {
                r = h.new Run(rate, Double.parseDouble(a.get("--seconds")), Double.parseDouble(a.get("--warmup"))).go(agg);
            } finally {
                agg.close();
            }
            print(r, header);
            header = false;
            for (String e : r.errors) allErrors.add("rate " + (long) rate + ": " + e);
            Thread.sleep(1000);   // let the mock drain before the next rate
        }
        if (!allErrors.isEmpty()) {
            System.out.println("first problems:");
            allErrors.stream().limit(10).forEach(e -> System.out.println("  " + e));
        }
        h.client.close();
        System.exit(0);
    }
}
