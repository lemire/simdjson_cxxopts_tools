namespace stage2 {

struct streaming_structural_parser: structural_parser {
  really_inline streaming_structural_parser(dom_parser_implementation &_parser, uint32_t _next_structural) : structural_parser(_parser, _next_structural) {}

  // override to add streaming
  WARN_UNUSED really_inline error_code start(ret_address finish_parser) {
    log_start();
    init(); // sets is_valid to false
    // Capacity ain't no thang for streaming, so we don't check it.
    // Advance to the first character as soon as possible
    advance_char();
    // Push the root scope (there is always at least one scope)
    if (start_document(finish_parser)) {
      return DEPTH_ERROR;
    }
    return SUCCESS;
  }

  // override to add streaming
  WARN_UNUSED really_inline error_code finish() {
    if ( past_end() ) {
      log_error("IMPOSSIBLE: past the end of the JSON!");
      return TAPE_ERROR;
    }
    end_document();
    if (depth != 0) {
      log_error("Unclosed objects or arrays!");
      return TAPE_ERROR;
    }
    if (parser.containing_scope[depth].tape_index != 0) {
      log_error("IMPOSSIBLE: root scope tape index did not start at 0!");
      return TAPE_ERROR;
    }
    bool finished = at_end();
    if (!finished) { log_value("(and has more)"); }
    return finished ? SUCCESS : SUCCESS_AND_HAS_MORE;
  }
};

} // namespace stage2

/************
 * The JSON is parsed to a tape, see the accompanying tape.md file
 * for documentation.
 ***********/
WARN_UNUSED error_code dom_parser_implementation::stage2(const uint8_t *_buf, size_t _len, dom::document &_doc, size_t &next_json) noexcept {
  this->buf = _buf;
  this->len = _len;
  this->tape = _doc.tape.get();
  this->string_buf = _doc.string_buf.get();
  static constexpr stage2::unified_machine_addresses addresses = INIT_ADDRESSES();
  stage2::streaming_structural_parser parser(*this, uint32_t(next_json));
  error_code result = parser.start(addresses.finish);
  if (result) { return result; }
  //
  // Read first value
  //
  switch (parser.current_char()) {
  case '{':
    FAIL_IF( parser.start_object(addresses.finish) );
    goto object_begin;
  case '[':
    FAIL_IF( parser.start_array(addresses.finish) );
    goto array_begin;
  case '"':
    FAIL_IF( parser.parse_string() );
    goto finish;
  case 't': case 'f': case 'n':
    FAIL_IF( parser.parse_single_atom() );
    goto finish;
  case '0': case '1': case '2': case '3': case '4':
  case '5': case '6': case '7': case '8': case '9':
    FAIL_IF(
      parser.with_space_terminated_copy([&](const uint8_t *copy, size_t idx) {
        return parser.parse_number(&copy[idx], false);
      })
    );
    goto finish;
  case '-':
    FAIL_IF(
      parser.with_space_terminated_copy([&](const uint8_t *copy, size_t idx) {
        return parser.parse_number(&copy[idx], true);
      })
    );
    goto finish;
  default:
    parser.log_error("Document starts with a non-value character");
    goto error;
  }

//
// Object parser parsers
//
object_begin:
  switch (parser.advance_char()) {
  case '"': {
    FAIL_IF( parser.parse_string(true) );
    goto object_key_parser;
  }
  case '}':
    parser.end_object();
    goto scope_end;
  default:
    parser.log_error("Object does not start with a key");
    goto error;
  }

object_key_parser:
  if (parser.advance_char() != ':' ) { parser.log_error("Missing colon after key in object"); goto error; }
  parser.increment_count();
  parser.advance_char();
  GOTO( parser.parse_value(addresses, addresses.object_continue) );

object_continue:
  switch (parser.advance_char()) {
  case ',':
    if (parser.advance_char() != '"' ) { parser.log_error("Key string missing at beginning of field in object"); goto error; }
    FAIL_IF( parser.parse_string(true) );
    goto object_key_parser;
  case '}':
    parser.end_object();
    goto scope_end;
  default:
    parser.log_error("No comma between object fields");
    goto error;
  }

scope_end:
  CONTINUE( parser.parser.ret_address[parser.depth] );

//
// Array parser parsers
//
array_begin:
  if (parser.advance_char() == ']') {
    parser.end_array();
    goto scope_end;
  }
  parser.increment_count();

main_array_switch:
  /* we call update char on all paths in, so we can peek at parser.c on the
   * on paths that can accept a close square brace (post-, and at start) */
  GOTO( parser.parse_value(addresses, addresses.array_continue) );

array_continue:
  switch (parser.advance_char()) {
  case ',':
    parser.increment_count();
    parser.advance_char();
    goto main_array_switch;
  case ']':
    parser.end_array();
    goto scope_end;
  default:
    parser.log_error("Missing comma between array values");
    goto error;
  }

finish:
  next_json = parser.next_structural_index();
  return parser.finish();

error:
  return parser.error();
}
