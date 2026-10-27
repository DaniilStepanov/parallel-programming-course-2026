package lab2;

import java.util.*;
import java.util.function.Supplier;

/** Programs under test. setup() spawns threads; check() returns null if the run is fine. */
public final class Targets {
    private Targets() { }

    public interface Target {
        void setup(Scheduler s);
        String check();
    }

    public static Supplier<Target> byName(String name) {
        return switch (name) {
            case "withdraw-v0" -> () -> new Withdraw(0);
            case "withdraw-v1" -> () -> new Withdraw(1);
            case "withdraw-v2" -> () -> new Withdraw(2);
            case "transfer" -> Transfer::new;
            default -> {
                if (name.startsWith("collector-")) yield collector(name.substring("collector-".length()));
                throw new IllegalArgumentException("unknown target " + name);
            }
        };
    }

    // ---------------- withdraw V0 / V1 / V2 ----------------

    static final class Withdraw implements Target {
        final int version;
        final Shared<Integer> sharedBalance = new Shared<>("balance", 100);   // V0: no lock
        int balance = 100;                                                     // V1, V2: under mu
        SchedLock mu;
        final boolean[] ok = new boolean[2];

        Withdraw(int version) { this.version = version; }

        boolean withdraw(int amt) {
            switch (version) {
                case 0 -> {
                    boolean ok = sharedBalance.get() >= amt;
                    Scheduler.point();
                    if (ok) sharedBalance.set(sharedBalance.get() - amt);
                    return ok;
                }
                case 1 -> {
                    mu.lock(); boolean ok = balance >= amt; mu.unlock();     // check
                    Scheduler.point();
                    if (ok) { mu.lock(); balance -= amt; mu.unlock(); }      // act
                    return ok;
                }
                default -> {
                    mu.lock();
                    if (balance < amt) { mu.unlock(); return false; }
                    Scheduler.point();
                    balance -= amt;
                    mu.unlock();
                    return true;
                }
            }
        }

        public void setup(Scheduler s) {
            mu = new SchedLock(s, "mu");
            for (int i = 0; i < 2; i++) { int k = i; s.spawn(() -> ok[k] = withdraw(100)); }
        }

        public String check() {
            int b = version == 0 ? sharedBalance.peek() : balance;
            int wins = (ok[0] ? 1 : 0) + (ok[1] ? 1 : 0);
            String state = "balance=" + b + " results=" + ok[0] + "," + ok[1];
            if (b < 0) return "OVERDRAFT " + state;
            if (100 - b != wins * 100) return "LOST UPDATE " + state;
            return null;
        }
    }

    // ---------------- transfer: lock order ABBA ----------------

    static final class Transfer implements Target {
        SchedLock a, b;
        int balA = 100, balB = 100;

        void transfer(SchedLock from, SchedLock to, boolean aToB) {
            from.lock();
            Scheduler.point();
            to.lock();
            if (aToB) { balA -= 10; balB += 10; } else { balB -= 10; balA += 10; }
            to.unlock();
            from.unlock();
        }

        public void setup(Scheduler s) {
            a = new SchedLock(s, "a"); b = new SchedLock(s, "b");
            s.spawn(() -> transfer(a, b, true));
            s.spawn(() -> transfer(b, a, false));
        }

        public String check() { return balA + balB == 200 ? null : "MONEY LOST"; }
    }

    // ---------------- double buffering from lab 1, one bucket ----------------
    // collector-<variant>-<writers>x<n>, variant: nostep3 | plain | acqrel | ok

    static Supplier<Target> collector(String spec) {
        String[] p = spec.split("-");
        String variant = p[0];
        int writers = 1, n = 1;
        if (p.length > 1) { String[] wn = p[1].split("x"); writers = Integer.parseInt(wn[0]); n = Integer.parseInt(wn[1]); }
        if (!Set.of("nostep3", "plain", "acqrel", "ok").contains(variant)) throw new IllegalArgumentException(variant);
        int w = writers, nn = n;
        return () -> new Collector(variant, w, nn);
    }

    static final class Collector implements Target {
        final boolean step3, plainInside, acqrel;
        final int writers, n;
        final Shared<Integer> active = new Shared<>("active", 0);
        final List<Shared<Integer>> inside = new ArrayList<>();
        final int[] insidePlain;
        final List<List<Shared<Long>>> cnt = new ArrayList<>();
        long global;

        Collector(String variant, int writers, int n) {
            this.writers = writers; this.n = n;
            step3 = variant.equals("ok") || variant.equals("acqrel");
            plainInside = variant.equals("plain");
            acqrel = variant.equals("acqrel");
            insidePlain = new int[writers];
            Arrays.fill(insidePlain, -1);
            for (int w = 0; w < writers; w++) {
                String W = "W" + (w + 1);
                inside.add(new Shared<>("inside[" + W + "]", -1));
                cnt.add(List.of(new Shared<>("cnt[" + W + "][0]", 0L), new Shared<>("cnt[" + W + "][1]", 0L)));
            }
        }

        int getInside(int w) { return plainInside ? insidePlain[w] : inside.get(w).peek(); }

        void setInside(int w, int v) {
            if (plainInside) { insidePlain[w] = v; return; }
            if (acqrel) inside.get(w).setRelease(v); else inside.get(w).set(v);
        }

        void record(int w) {
            int b;
            while (true) {
                b = acqrel ? active.getAcquire() : active.get();
                setInside(w, b);
                if (!step3 || active.get() == b) break;    // step 3: did the reader flip meanwhile?
                setInside(w, -1);
            }
            Shared<Long> c = cnt.get(w).get(b);
            c.set(c.get() + 1);
            setInside(w, -1);
        }

        void snapshot() {
            int old = active.get();
            active.set(1 - old);
            for (int w = 0; w < writers; w++) { int k = w; Scheduler.await(() -> getInside(k) != old); }
            for (int w = 0; w < writers; w++) {
                Shared<Long> c = cnt.get(w).get(old);
                global += c.get();
                c.set(0L);
            }
        }

        public void setup(Scheduler s) {
            for (int w = 0; w < writers; w++) { int k = w; s.spawn(() -> { for (int i = 0; i < n; i++) record(k); }); }
            s.spawn(this::snapshot);
        }

        public String check() {
            long total = global;
            for (List<Shared<Long>> c : cnt) total += c.get(0).peek() + c.get(1).peek();
            long expected = (long) writers * n;
            if (total < expected) return "LOST " + (expected - total) + " record(s): counted " + total + " of " + expected;
            if (total > expected) return "DUPLICATED " + (total - expected) + ": counted " + total + " of " + expected;
            return null;
        }
    }
}
