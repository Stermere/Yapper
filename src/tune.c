/* tune.c - the tunables as variables, for a `make TUNE=on` build. Otherwise they are constants
 * and there is nothing here. */
#include "tune.h"

#ifdef YAPPER_TUNE

#include <string.h>

#include "search.h"

#define X(name, value, min, max) int name = (value);
TUNABLES(X)
#undef X

static const struct {
    const char *name;
    int *value;
    int min, max;
} Tunables[] = {
#define X(name, value, min, max) {#name, &name, (min), (max)},
    TUNABLES(X)
#undef X
};

int tune_count(void) { return (int)(sizeof(Tunables) / sizeof(Tunables[0])); }

void tune_info(int i, const char **name, int *value, int *min, int *max) {
    *name  = Tunables[i].name;
    *value = *Tunables[i].value;
    *min   = Tunables[i].min;
    *max   = Tunables[i].max;
}

bool tune_set(const char *name, size_t length, int value) {
    for (int i = 0; i < tune_count(); i++) {
        if (strlen(Tunables[i].name) == length && strncmp(name, Tunables[i].name, length) == 0) {
            const int min = Tunables[i].min, max = Tunables[i].max;
            *Tunables[i].value = value < min ? min : value > max ? max : value;
            search_retune();
            return true;
        }
    }
    return false;
}

#endif
