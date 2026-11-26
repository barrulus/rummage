// A harmless ELF executable whose behavior does not depend on its filename or environment.
#ifndef RUMMAGE_FIXTURE_EXIT
#  define RUMMAGE_FIXTURE_EXIT 0
#endif
int main() { return RUMMAGE_FIXTURE_EXIT; }
