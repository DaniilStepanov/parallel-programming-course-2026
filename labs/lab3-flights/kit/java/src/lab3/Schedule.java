package lab3;

import java.io.IOException;
import java.nio.ByteOrder;
import java.nio.MappedByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.util.HashMap;
import java.util.Map;

/**
 * Read-only view of schedule.bin (format in the lab README). The file is memory-mapped, so opening is instant;
 * all getters are thread-safe. flightId = record index.
 */
public final class Schedule {
    private static final int HEADER = 16;
    private static final int RECORD = 20;

    public final int nAirports, nAirlines, nFlights;
    private final String[] airports, airlines;
    private final Map<String, Integer> airportIds = new HashMap<>(), airlineIds = new HashMap<>();
    private final MappedByteBuffer b;
    private final int recordsStart;

    private Schedule(MappedByteBuffer b) throws IOException {
        this.b = b;
        b.order(ByteOrder.LITTLE_ENDIAN);
        if (b.get(0) != 'F' || b.get(1) != 'L' || b.get(2) != 'T' || b.get(3) != '1') throw new IOException("not a FLT1 file");
        nAirports = b.getInt(4);
        nAirlines = b.getInt(8);
        nFlights = b.getInt(12);
        airports = new String[nAirports];
        airlines = new String[nAirlines];
        int p = HEADER;
        for (int i = 0; i < nAirports; i++, p += 3) {
            airports[i] = new String(new char[] {(char) b.get(p), (char) b.get(p + 1), (char) b.get(p + 2)});
            airportIds.put(airports[i], i);
        }
        for (int i = 0; i < nAirlines; i++, p += 2) {
            airlines[i] = new String(new char[] {(char) b.get(p), (char) b.get(p + 1)});
            airlineIds.put(airlines[i], i);
        }
        recordsStart = p;
    }

    public static Schedule open(Path file) throws IOException {
        try (FileChannel ch = FileChannel.open(file, StandardOpenOption.READ)) {
            return new Schedule(ch.map(FileChannel.MapMode.READ_ONLY, 0, ch.size()));
        }
    }

    private int at(int flightId) { return recordsStart + flightId * RECORD; }

    public int dep(int flightId) { return b.getInt(at(flightId)); }
    public int arr(int flightId) { return b.getInt(at(flightId) + 4); }
    public int from(int flightId) { return b.getShort(at(flightId) + 8) & 0xFFFF; }
    public int to(int flightId) { return b.getShort(at(flightId) + 10) & 0xFFFF; }
    public int airline(int flightId) { return b.getShort(at(flightId) + 12) & 0xFFFF; }
    public int flightNo(int flightId) { return b.getShort(at(flightId) + 14) & 0xFFFF; }
    public int price(int flightId) { return b.getInt(at(flightId) + 16); }

    public String airportCode(int id) { return airports[id]; }
    public String airlineCode(int id) { return airlines[id]; }
    /** @return id or -1 */
    public int airportId(String code) { return airportIds.getOrDefault(code, -1); }
    /** @return id or -1 */
    public int airlineId(String code) { return airlineIds.getOrDefault(code, -1); }

    public Flight flight(int flightId) {
        return new Flight(flightId, airlines[airline(flightId)], airports[from(flightId)], airports[to(flightId)],
                dep(flightId), arr(flightId), price(flightId));
    }
}
