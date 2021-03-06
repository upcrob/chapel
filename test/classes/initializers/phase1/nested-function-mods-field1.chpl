// This test exercises the case where an initializer defines a function within
// itself, but the function modifies a field.
class Foo {
  var field: int;

  proc init(val) {
    field = val;
    nested(); // This call shouldn't be allowed in Phase 1
    proc nested() {
      field = -field; // because this modifies a field
    }
    super.init();
  }
}

proc main() {
  var f = new Foo(13);
  writeln(f);
  delete f;
}
