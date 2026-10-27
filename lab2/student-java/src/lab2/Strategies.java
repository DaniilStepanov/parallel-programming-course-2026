package lab2;

import java.util.List;

/* Стратегии выбора следующего потока.
 * FirstAlive — этап 1, Replay — этап 2, RandomPick — этап 3, Dfs — этап 4 (ограничение bound — этап 5).
 * enabled — номера потоков, которые могут сделать шаг, по возрастанию; last — прошлый выбранный (-1 в начале). */
public final class Strategies {
    private Strategies() { }

    /** Всегда первый поток из enabled. */
    public static final class FirstAlive implements Strategy {
        public int pick(List<Integer> enabled, int last) {
            throw new UnsupportedOperationException("TODO этап 1: FirstAlive.pick");
        }
    }

    /** Проигрывает трассу: на i-м решении выбирает поток trace[i] ('A' -> 0, 'B' -> 1). */
    public static final class Replay implements Strategy {
        public Replay(String trace) { }

        /** Сбросить позицию в трассе перед прогоном. */
        public void beginRun() { }

        public int pick(List<Integer> enabled, int last) {
            throw new UnsupportedOperationException("TODO этап 2: Replay.pick");
        }
    }

    /** Случайный поток: enabled.get(rnd.nextInt(enabled.size())), rnd = new java.util.Random(seed). */
    public static final class RandomPick implements Strategy {
        public RandomPick(long seed) { }

        public int pick(List<Integer> enabled, int last) {
            throw new UnsupportedOperationException("TODO этап 3: RandomPick.pick");
        }
    }

    /** Stateless DFS по дереву выборов: каждый прогон с нуля, повторяет префикс прошлого и берёт следующий вариант.
     *  bound — максимум вытеснений (этап 5); для dfs runner передаёт Integer.MAX_VALUE. */
    public static final class Dfs implements Strategy {
        public Dfs(int bound) { }

        /** Подготовиться к новому прогону (позиция в пути, счётчик вытеснений). */
        public void beginRun() { }

        public int pick(List<Integer> enabled, int last) {
            throw new UnsupportedOperationException("TODO этап 4: Dfs.pick");
        }

        /** Выбрать следующий путь; false — дерево обойдено. */
        public boolean endRun() {
            throw new UnsupportedOperationException("TODO этап 4: Dfs.endRun");
        }
    }
}
