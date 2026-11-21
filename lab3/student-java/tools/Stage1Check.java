import java.nio.file.*;
import java.util.*;

/**
 * Compares stage 1 output with the reference, ignoring ms=.
 *   java Stage1Check.java <your-output.txt> [expected-stage1.txt]
 * Exit code 0 if every query matches.
 */
public class Stage1Check {
    public static void main(String[] args) throws Exception {
        if (args.length < 1) { System.err.println("usage: java Stage1Check.java <your-output.txt> [expected-stage1.txt]"); System.exit(2); }
        Path expectedFile = args.length > 1 ? Path.of(args[1])
                : Path.of(Stage1Check.class.getProtectionDomain().getCodeSource().getLocation().toURI()).getParent().resolve("../../data/expected-stage1.txt").normalize();
        if (!Files.exists(expectedFile)) expectedFile = Path.of("expected-stage1.txt");
        Map<String, List<String>> want = parse(Files.readAllLines(expectedFile));
        Map<String, List<String>> got = parse(Files.readAllLines(Path.of(args[0])));
        int ok = 0, bad = 0;
        for (var e : want.entrySet()) {
            List<String> g = got.get(e.getKey());
            if (g == null) { System.out.println(e.getKey() + ": MISSING"); bad++; continue; }
            List<String> w = e.getValue();
            if (!w.get(0).equals(g.get(0))) {
                System.out.println(e.getKey() + ": header differs\n  expected: " + w.get(0) + "\n  got:      " + g.get(0));
                bad++;
                continue;
            }
            int line = mismatch(w, g);
            if (line >= 0) {
                System.out.println(e.getKey() + ": top-10 line " + line + " differs\n  expected: "
                        + (line < w.size() ? w.get(line).trim() : "<none>") + "\n  got:      " + (line < g.size() ? g.get(line).trim() : "<none>"));
                bad++;
            } else ok++;
        }
        System.out.printf("%d/%d queries match%n", ok, want.size());
        System.exit(bad == 0 ? 0 : 1);
    }

    static int mismatch(List<String> w, List<String> g) {
        for (int i = 1; i < Math.max(w.size(), g.size()); i++)
            if (i >= w.size() || i >= g.size() || !w.get(i).trim().equals(g.get(i).trim())) return i;
        return -1;
    }

    /** Query id -> [header without ms, route lines...]. */
    static Map<String, List<String>> parse(List<String> lines) {
        Map<String, List<String>> m = new LinkedHashMap<>();
        List<String> cur = null;
        for (String l : lines) {
            if (l.isBlank()) continue;
            if (l.startsWith("Q")) {
                cur = new ArrayList<>();
                cur.add(l.replaceAll("\\s+ms=\\S+", "").trim().replaceAll("\\s+", " "));
                m.put(l.split("\\s+")[0], cur);
            } else if (cur != null) cur.add(l.trim().replaceAll("\\s+", " "));
        }
        return m;
    }
}
