package lab3;

import java.io.*;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

/**
 * Client of the airline mock. Thread-safe; create one per process and share it.
 *
 * <p>Requests go over a few TCP connections (all requests to one airline over the same one). Each connection
 * has one reader thread that completes the answers. The client does not retry, does not limit and has no deadlines of its own: that is your job.
 *
 * <p>Every request counts against the airline's limit until it is answered or cancelled. A request that is
 * neither answered nor cancelled when the search is done is an <b>orphan</b>.
 */
public final class AirlineClient implements AutoCloseable {
    private final Connection[] conns;

    public AirlineClient(String host, int port, int connections) throws IOException {
        conns = new Connection[connections];
        for (int i = 0; i < connections; i++) conns[i] = new Connection(host, port, i);
    }

    /** localhost:7700, 8 connections. */
    public AirlineClient() throws IOException { this("127.0.0.1", 7700, 8); }

    /**
     * Blocking request. Returns the answer, or {@link PriceResult#TIMEOUT} if none came within {@code timeout};
     * in that case, and on interrupt, the request is cancelled at the airline (CANCEL is sent).
     */
    public PriceResult get(PriceKey key, long searchId, Duration timeout) throws InterruptedException {
        Call c = start(key, searchId);
        try {
            return c.future.get(Math.max(0, timeout.toNanos()), TimeUnit.NANOSECONDS);
        } catch (TimeoutException e) {
            return c.cancel() ? PriceResult.TIMEOUT : c.future.getNow(PriceResult.TIMEOUT);
        } catch (InterruptedException e) {
            c.cancel();
            throw e;
        } catch (ExecutionException e) {
            throw new IllegalStateException(e.getCause());
        }
    }

    /**
     * Asynchronous request.
     *
     * <p>The future completes on the client's reader thread. Only {@link PriceCall#cancel()} cancels the request
     * at the airline. {@code future().cancel(true)}, {@code orTimeout(...)}, {@code completeOnTimeout(...)}
     * complete the future object only: the airline keeps working on the request, it still counts against the
     * airline's limit, and it becomes an orphan if the search is done before the airline answers.
     */
    public PriceCall getAsync(PriceKey key, long searchId) { return start(key, searchId); }

    private Call start(PriceKey key, long searchId) {
        // One airline always uses the same connection: a CANCEL then reaches the mock before any later request
        // to that airline, so freeing a limit slot right after cancel() is safe.
        Connection conn = conns[Math.floorMod(key.airline().hashCode(), conns.length)];
        return conn.send(key, searchId);
    }

    @Override public void close() {
        for (Connection c : conns) c.close();
    }

    private static final class Call implements PriceCall {
        final Connection conn;
        final long reqId;
        final CompletableFuture<PriceResult> future = new CompletableFuture<>();

        Call(Connection conn, long reqId) { this.conn = conn; this.reqId = reqId; }

        @Override public CompletableFuture<PriceResult> future() { return future; }

        @Override public boolean cancel() {
            if (conn.calls.remove(reqId) == null) return false;   // already answered
            conn.write("CANCEL " + reqId);
            future.completeExceptionally(new CancellationException("request " + reqId + " cancelled"));
            return true;
        }
    }

    private static final class Connection {
        final Socket sock;
        final Writer out;
        final ConcurrentHashMap<Long, Call> calls = new ConcurrentHashMap<>();
        final AtomicLong ids = new AtomicLong();
        final Thread reader;

        Connection(String host, int port, int n) throws IOException {
            sock = new Socket();
            sock.setTcpNoDelay(true);
            sock.connect(new InetSocketAddress(host, port));
            out = new BufferedWriter(new OutputStreamWriter(sock.getOutputStream(), StandardCharsets.US_ASCII), 1 << 16);
            reader = new Thread(this::readLoop, "airline-client-reader-" + n);
            reader.setDaemon(true);
            reader.start();
        }

        Call send(PriceKey k, long searchId) {
            long id = ids.incrementAndGet();
            Call c = new Call(this, id);
            calls.put(id, c);
            write("PRICE " + id + " " + searchId + " " + k.airline() + " " + k.from() + " " + k.to() + " " + k.day());
            return c;
        }

        void write(String line) {
            try {
                synchronized (out) {
                    out.write(line);
                    out.write('\n');
                    out.flush();
                }
            } catch (IOException e) {
                throw new UncheckedIOException("airline mock connection lost", e);
            }
        }

        void readLoop() {
            try (BufferedReader in = new BufferedReader(new InputStreamReader(sock.getInputStream(), StandardCharsets.US_ASCII), 1 << 16)) {
                String line;
                while ((line = in.readLine()) != null) {
                    int sp = line.indexOf(' ');
                    long id = Long.parseLong(line, 0, sp, 10);
                    Call c = calls.remove(id);
                    if (c == null) continue;   // cancelled meanwhile
                    c.future.complete(parse(line, sp + 1));
                }
            } catch (IOException ignored) {
            } finally {
                for (Call c : calls.values()) c.future.completeExceptionally(new IOException("airline mock connection closed"));
            }
        }

        static PriceResult parse(String line, int p) {
            if (line.startsWith("ERR", p)) return PriceResult.ERR;
            if (line.startsWith("LIMIT", p)) return PriceResult.LIMIT;
            p += 2;   // "OK"
            int n = 0;
            for (int i = p; i < line.length(); i++) if (line.charAt(i) == ':') n++;
            int[] ids = new int[n], prices = new int[n];
            int k = 0, i = p;
            while (k < n) {
                i++;   // space
                int colon = line.indexOf(':', i), end = line.indexOf(' ', colon);
                if (end < 0) end = line.length();
                ids[k] = Integer.parseInt(line, i, colon, 10);
                prices[k] = Integer.parseInt(line, colon + 1, end, 10);
                k++;
                i = end;
            }
            return new PriceResult(PriceResult.Kind.OK, ids, prices);
        }

        void close() {
            try { sock.close(); } catch (IOException ignored) {}
        }
    }
}
