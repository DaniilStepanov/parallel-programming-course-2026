package lab2;

import java.util.function.Supplier;

/** lab2 <target> <strategy> [arg]
 *  strategies: first | random <seed> | randoms <count> | replay <trace> | dfs | bounded <K>
 *  add "log" at the end of replay/first/random to print the steps of the run. */
public final class Main {
    record Outcome(String trace, String failure) { }

    static Outcome runOnce(Supplier<Targets.Target> factory, Strategy st, boolean log) {
        Scheduler s = new Scheduler();
        Targets.Target t = factory.get();
        s.logging = log;
        t.setup(s);
        st.beginRun();
        s.run(st);
        if (log) s.log.forEach(l -> System.out.println("  " + l));
        String failure = s.deadlock != null ? "DEADLOCK " + s.deadlock : t.check();
        return new Outcome(s.traceString(), failure);
    }

    static void single(Supplier<Targets.Target> f, Strategy st, boolean log) {
        Outcome o = runOnce(f, st, log);
        System.out.println("schedule " + o.trace + " -> " + (o.failure == null ? "ok" : o.failure));
    }

    static void explore(Supplier<Targets.Target> f, int bound) {
        Strategies.Dfs dfs = new Strategies.Dfs(bound);
        long t0 = System.nanoTime();
        int explored = 0, fail = 0;
        Outcome first = null;
        do {
            Outcome o = runOnce(f, dfs, false);
            explored++;
            if (o.failure != null) { fail++; if (first == null) first = o; }
        } while (dfs.endRun());
        double sec = (System.nanoTime() - t0) / 1e9;
        System.out.printf("explored %d schedules, %d fail  (%.2f s)%n", explored, fail, sec);
        if (first != null) System.out.println("first failing: " + first.trace + " -> " + first.failure);
    }

    static void randoms(Supplier<Targets.Target> f, int count) {
        int fail = 0; long firstSeed = -1;
        for (long seed = 0; seed < count; seed++) {
            Outcome o = runOnce(f, new Strategies.RandomPick(seed), false);
            if (o.failure != null) { fail++; if (firstSeed < 0) firstSeed = seed; }
        }
        System.out.printf("random: %d seeds, %d fail (%.1f%%)%n", count, fail, 100.0 * fail / count);
        if (firstSeed >= 0) {
            Outcome o = runOnce(f, new Strategies.RandomPick(firstSeed), false);
            System.out.println("first failing seed " + firstSeed + ": " + o.trace + " -> " + o.failure);
            int same = 0;
            for (int i = 0; i < 10; i++) {
                Outcome r = runOnce(f, new Strategies.Replay(o.trace), false);
                if (r.trace.equals(o.trace) && o.failure.equals(r.failure)) same++;
            }
            System.out.println("replay " + o.trace + ": same failure " + same + "/10");
        }
    }

    public static void main(String[] a) {
        if (a.length < 2) { System.out.println("usage: lab2 <target> <strategy> [arg] [log]"); return; }
        Supplier<Targets.Target> f = Targets.byName(a[0]);
        boolean log = a[a.length - 1].equals("log");
        switch (a[1]) {
            case "first" -> single(f, new Strategies.FirstAlive(), log);
            case "random" -> single(f, new Strategies.RandomPick(Long.parseLong(a[2])), log);
            case "replay" -> single(f, new Strategies.Replay(a[2]), log);
            case "randoms" -> randoms(f, Integer.parseInt(a[2]));
            case "dfs" -> explore(f, Integer.MAX_VALUE);
            case "bounded" -> explore(f, Integer.parseInt(a[2]));
            default -> throw new IllegalArgumentException("unknown strategy " + a[1]);
        }
    }
}
