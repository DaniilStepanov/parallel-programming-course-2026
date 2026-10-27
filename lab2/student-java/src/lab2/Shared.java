package lab2;

/* Этап 1: общая переменная. Перед каждым get/set — точка переключения (пауза планировщика),
 * после — строка в лог: чтение "<имя> -> <значение>", запись "<имя> = <значение>".
 * getAcquire/setRelease для планировщика ведут себя так же, как get/set. */
public final class Shared<T> {
    private final String name;
    private T value;

    public Shared(String name, T value) { this.name = name; this.value = value; }

    /** Чтение с паузой перед ним. */
    public T get() {
        throw new UnsupportedOperationException("TODO этап 1: Shared.get");
    }

    /** Запись с паузой перед ней. */
    public void set(T v) {
        throw new UnsupportedOperationException("TODO этап 1: Shared.set");
    }

    /** То же, что get(): планировщик не моделирует порядки памяти. */
    public T getAcquire() {
        throw new UnsupportedOperationException("TODO этап 1: Shared.getAcquire");
    }

    /** То же, что set(v). */
    public void setRelease(T v) {
        throw new UnsupportedOperationException("TODO этап 1: Shared.setRelease");
    }

    /** Чтение без паузы и без лога: для проверок после прогона и условий await. */
    public T peek() { return value; }
}
