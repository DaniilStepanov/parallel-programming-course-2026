package lab3;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashMap;
import java.util.Map;

/**
 * Everything the harness gives your aggregator.
 *
 * @param client   shared airline client
 * @param schedule schedule.bin (not needed for stage 2, there if you want it)
 * @param limits   max unanswered requests per airline, from airlines.conf
 * @param options  {@code --opt key=value} from the command line, e.g. pool size
 */
public record Kit(AirlineClient client, Schedule schedule, Map<String, Integer> limits, Map<String, String> options) {

    public int limit(String airline) {
        Integer l = limits.get(airline);
        if (l == null) throw new IllegalArgumentException("unknown airline " + airline);
        return l;
    }

    public int intOption(String name, int fallback) {
        String v = options.get(name);
        return v == null ? fallback : Integer.parseInt(v);
    }

    static Map<String, Integer> loadLimits(Path airlinesConf) throws IOException {
        Map<String, Integer> m = new HashMap<>();
        for (String line : Files.readAllLines(airlinesConf)) {
            line = line.strip();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] p = line.split("\\s+");
            m.put(p[0], Integer.parseInt(p[1]));
        }
        return Map.copyOf(m);
    }
}
