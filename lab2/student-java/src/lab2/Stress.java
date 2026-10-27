package lab2;

import java.util.concurrent.CyclicBarrier;
import java.util.concurrent.locks.ReentrantLock;

/** Stage 0: withdraw V1 or V2 with real threads and a real lock, no scheduler.
 *  stress <runs> [none|yield|sleep] [v1|v2]  — what to put into window(), which version. */
public final class Stress {
    static int balance;
    static final ReentrantLock mu = new ReentrantLock();

    static boolean v2;

    static void window(String window) throws InterruptedException {
        switch (window) { case "yield" -> Thread.yield(); case "sleep" -> Thread.sleep(1); default -> { } }
    }

    static boolean withdraw(int amt, String window) throws InterruptedException {
        if (v2) {
            mu.lock();
            try { if (balance < amt) return false; window(window); balance -= amt; return true; }
            finally { mu.unlock(); }
        }
        mu.lock(); boolean ok = balance >= amt; mu.unlock();
        window(window);
        if (ok) { mu.lock(); balance -= amt; mu.unlock(); }
        return ok;
    }

    public static void main(String[] a) throws Exception {
        int runs = Integer.parseInt(a[0]);
        String window = a.length > 1 ? a[1] : "none";
        v2 = a.length > 2 && a[2].equals("v2");
        CyclicBarrier start = new CyclicBarrier(3), end = new CyclicBarrier(3);
        Runnable body = () -> {
            try { for (int i = 0; i < runs; i++) { start.await(); withdraw(100, window); end.await(); } }
            catch (Exception e) { throw new RuntimeException(e); }
        };
        new Thread(body).start(); new Thread(body).start();
        int bad = 0;
        for (int i = 0; i < runs; i++) {
            balance = 100;
            start.await();                        // both threads start withdraw together
            end.await();                          // both withdraws finished
            if (balance < 0) bad++;
        }
        System.out.printf("stress %s, window=%s: %d runs, %d overdrafts (%.2f%%)%n", v2 ? "V2" : "V1", window, runs, bad, 100.0 * bad / runs);
        System.exit(0);
    }
}
