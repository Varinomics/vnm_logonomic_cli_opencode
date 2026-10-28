// Provider-local final_paragraph_edges evidence follows the SDK attention-kind
// contract. Complete native text parts are scanned before anything is queued.
import { Buffer } from "node:buffer"

const edge_bytes = 128
const whitespace = (character) => " \t\r\n".includes(character)

class Paragraph_edges {
  prefix = ""
  suffix = ""
  trimmed_prefix = ""
  trimmed_suffix = ""
  prefix_bytes = 0
  suffix_bytes = 0
  prefix_full = false

  append(character) {
    if (!this.prefix && whitespace(character)) return
    const size = Buffer.byteLength(character, "utf8")
    if (!this.prefix_full) {
      if (this.prefix_bytes + size <= edge_bytes) {
        this.prefix += character
        this.prefix_bytes += size
      } else {
        this.prefix_full = true
      }
    }
    this.suffix += character
    this.suffix_bytes += size
    while (this.suffix_bytes > edge_bytes) {
      const first = String.fromCodePoint(this.suffix.codePointAt(0))
      this.suffix = this.suffix.slice(first.length)
      this.suffix_bytes -= Buffer.byteLength(first, "utf8")
    }
    // Whitespace may be internal once another native part arrives. Keep raw
    // edges, but expose only the snapshot ending at the last nonblank scalar.
    if (!whitespace(character)) {
      this.trimmed_prefix = this.prefix
      this.trimmed_suffix = this.suffix
    }
  }

  clone() {
    return Object.assign(new Paragraph_edges(), this)
  }

  evidence() {
    if (!this.trimmed_prefix) return
    return {
      scope: "final_paragraph_edges", context: "prose",
      prefix: this.trimmed_prefix, suffix: this.trimmed_suffix,
    }
  }
}

export class Assistant_reply_evidence {
  m_valid = true
  m_paragraph = new Paragraph_edges()
  m_next_paragraph = true
  m_prose = false
  m_fence = ""
  m_fence_width = 0
  m_separator = ""
  m_after_cr = false
  m_line

  start_line() {
    const candidate = this.m_next_paragraph
      ? new Paragraph_edges() : this.m_paragraph.clone()
    if (!this.m_next_paragraph) {
      for (const character of this.m_separator) candidate.append(character)
    }
    this.m_line = {
      candidate, blank: true, indentation: 0, tab: false, initial: "",
      marker: "", width: 0, marker_run: false, tail: false, tail_backtick: false,
    }
  }

  append_character(character) {
    if (!this.m_line) this.start_line()
    const line = this.m_line
    line.candidate.append(character)
    if (line.blank && (character === " " || character === "\t")) {
      line.indentation = Math.min(4, line.indentation + 1)
      line.tab ||= character === "\t"
      return
    }
    if (line.blank) {
      line.blank = false
      line.initial = character
      if (line.indentation < 4 && !line.tab && (character === "`" || character === "~")) {
        line.marker = character
        line.width = 1
        line.marker_run = true
      }
      return
    }
    if (line.marker_run && character === line.marker) {
      ++line.width
      return
    }
    line.marker_run = false
    line.tail ||= character !== " " && character !== "\t"
    line.tail_backtick ||= character === "`"
  }

  finish_line() {
    const line = this.m_line
    if (this.m_fence) {
      this.m_prose = false
      if (line?.marker === this.m_fence && line.width >= this.m_fence_width && !line.tail) {
        this.m_fence = ""
      }
      this.m_next_paragraph = true
    } else if (!line || line.blank) {
      this.m_next_paragraph = true
    } else if (line.width >= 3 && (line.marker !== "`" || !line.tail_backtick)) {
      this.m_fence = line.marker
      this.m_fence_width = line.width
      this.m_prose = false
      this.m_next_paragraph = true
    } else {
      if (this.m_next_paragraph) {
        this.m_prose = line.indentation < 4 && !line.tab &&
          ![">", '"', "`"].includes(line.initial)
      }
      if (line.initial === ">") this.m_prose = false
      this.m_paragraph = line.candidate
      this.m_next_paragraph = false
    }
    this.m_line = undefined
  }

  append(text) {
    if (!this.m_valid) return
    if (typeof text !== "string") {
      this.m_valid = false
      return
    }
    for (const character of text) {
      const scalar = character.codePointAt(0)
      if (scalar === 0 || (scalar >= 0xd800 && scalar <= 0xdfff)) {
        this.m_valid = false
        return
      }
      if (this.m_after_cr && character === "\n") {
        this.m_separator = "\r\n"
        this.m_after_cr = false
        continue
      }
      this.m_after_cr = character === "\r"
      if (character === "\r" || character === "\n") {
        this.finish_line()
        this.m_separator = character
      } else {
        this.append_character(character)
      }
    }
  }

  snapshot() {
    if (!this.m_valid) return
    // A part boundary is not a line/paragraph boundary. Finalize a copy so the
    // next part can continue this same line, fence marker or quoted paragraph.
    const completed = Object.assign(new Assistant_reply_evidence(), this)
    if (completed.m_line) completed.finish_line()
    return completed.m_prose ? completed.m_paragraph.evidence() : undefined
  }
}
