-- Highly experimental Space CoAP Lua Dissector (CoAP with 24-bit Message ID and payload_length option)
-- Copyright (C) 2025  Michael Karpov
--
-- This program is free software: you can redistribute it and/or modify
-- it under the terms of the GNU Affero General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or any later version.
--
-- This program is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU Affero General Public License for more details.
--
-- You should have received a copy of the GNU Affero General Public License
-- along with this program.  If not, see <https://www.gnu.org/licenses/>.

-- Define the new protocol
local spacecoap_proto = Proto("spacecoap", "Space CoAP Protocol")

-- Value string tables for types and codes (for human-readable display in Wireshark)
local coap_type_names = {
  [0] = "Confirmable (CON)",
  [1] = "Non-confirmable (NON)",
  [2] = "Acknowledgement (ACK)",
  [3] = "Reset (RES)"
}
local coap_code_names = {
  [0]   = "Empty (0.00)",
  [1]   = "GET (0.01)",
  [2]   = "POST (0.02)",
  [3]   = "PUT (0.03)",
  [4]   = "DELETE (0.04)",
  [65]  = "2.01 Created",
  [66]  = "2.02 Deleted",
  [67]  = "2.03 Valid",
  [68]  = "2.04 Changed",
  [69]  = "2.05 Content",
  [95]  = "2.31 Continue",
  [128] = "4.00 Bad Request",
  [129] = "4.01 Unauthorized",
  [130] = "4.02 Bad Option",
  [131] = "4.03 Forbidden",
  [132] = "4.04 Not Found",
  [133] = "4.05 Method Not Allowed",
  [134] = "4.06 Not Acceptable",
  [136] = "4.08 Request Entity Incomplete",
  [140] = "4.12 Precondition Failed",
  [141] = "4.13 Request Entity Too Large",
  [143] = "4.15 Unsupported Content-Format",
  [160] = "5.00 Internal Server Error",
  [161] = "5.01 Not Implemented",
  [162] = "5.02 Bad Gateway",
  [163] = "5.03 Service Unavailable",
  [164] = "5.04 Gateway Timeout",
  [165] = "5.05 Proxying Not Supported"
}

-- Define protocol fields for Wireshark's dissection tree
local f = spacecoap_proto.fields
f.version            = ProtoField.uint8  ("spacecoap.version",            "Version",           base.DEC, nil, 0xC0)
f.type               = ProtoField.uint8  ("spacecoap.type",               "Type",              base.DEC, coap_type_names, 0x30)
f.tkl                = ProtoField.uint8  ("spacecoap.tkl",                "Token Length",      base.DEC, nil, 0x0F)
f.code               = ProtoField.uint8  ("spacecoap.code",               "Code",              base.DEC, coap_code_names)
f.mid                = ProtoField.uint24 ("spacecoap.mid",                "Message ID",        base.DEC)         -- 24‑bit MID
f.token              = ProtoField.bytes  ("spacecoap.token",              "Token")
f.payload            = ProtoField.bytes  ("spacecoap.payload",            "Payload")
-- CoAP option fields
f.opt_if_match       = ProtoField.bytes  ("spacecoap.opt.if_match",       "If-Match")
f.opt_uri_host       = ProtoField.string ("spacecoap.opt.uri_host",       "Uri-Host")
f.opt_etag           = ProtoField.bytes  ("spacecoap.opt.etag",           "ETag")
f.opt_if_none_match  = ProtoField.none   ("spacecoap.opt.if_none_match",  "If-None-Match")
f.opt_observe        = ProtoField.uint24 ("spacecoap.opt.observe",        "Observe")
f.opt_uri_port       = ProtoField.uint16 ("spacecoap.opt.uri_port",       "Uri-Port")
f.opt_location_path  = ProtoField.string ("spacecoap.opt.location_path",  "Location-Path")
f.opt_uri_path       = ProtoField.string ("spacecoap.opt.uri_path",       "Uri-Path")
f.opt_content_format = ProtoField.uint16 ("spacecoap.opt.content_format", "Content-Format")
f.opt_max_age        = ProtoField.uint32 ("spacecoap.opt.max_age",        "Max-Age")
f.opt_uri_query      = ProtoField.string ("spacecoap.opt.uri_query",      "Uri-Query")
f.opt_accept         = ProtoField.uint16 ("spacecoap.opt.accept",         "Accept")
f.opt_location_query = ProtoField.string ("spacecoap.opt.location_query", "Location-Query")
f.opt_block2         = ProtoField.uint32 ("spacecoap.opt.block2",         "Block2")
f.opt_block1         = ProtoField.uint32 ("spacecoap.opt.block1",         "Block1")
f.opt_size2          = ProtoField.uint32 ("spacecoap.opt.size2",          "Size2")
f.opt_proxy_uri      = ProtoField.string ("spacecoap.opt.proxy_uri",      "Proxy-URI")
f.opt_proxy_scheme   = ProtoField.string ("spacecoap.opt.proxy_scheme",   "Proxy-Scheme")
f.opt_size1          = ProtoField.uint32 ("spacecoap.opt.size1",          "Size1")
f.opt_payload_length = ProtoField.uint32("spacecoap.opt.payload_length", "Payload-Length")

-- Known CoAP content formats for display 
local content_format_names = {
  [0]  = "text/plain",
  [40] = "application/link-format",
  [41] = "application/xml",
  [42] = "application/octet-stream",
  [47] = "application/exi",
  [50] = "application/json"
}

-- Main dissector function
function spacecoap_proto.dissector(buffer, pinfo, tree)
  local length = buffer:len()
  if length < 5 then return 0 end

  local b0       = buffer(0,1):uint()
  local version  = math.floor(b0 / 64) 
  if version ~= 1 then return 0 end 

  local type_val     = math.floor((b0 % 64) / 16) 
  local token_length =  b0 % 16 
  local code_val = buffer(1,1):uint()
  local code_str = coap_code_names[code_val] or string.format("0x%02X", code_val)
  local mid_val = buffer(2,3):uint()

  pinfo.cols.protocol = "Space CoAP"

  local token_str = "<none>"
  if token_length > 0 then
    token_str = buffer(5, token_length):bytes():tohex()
  end
  
  pinfo.cols.info = string.format(
    "%s, %s, MID=%d, Token=%s",
    coap_type_names[type_val],
    code_str,
    mid_val,
    token_str
  )

  local subtree = tree:add(spacecoap_proto, buffer(), "Space CoAP")
  subtree:add(f.version, buffer(0,1))
  subtree:add(f.type,    buffer(0,1))
  subtree:add(f.tkl,     buffer(0,1))
  subtree:add(f.code,    buffer(1,1))
  subtree:add(f.mid,     buffer(2,3))

  local offset = 5
  if token_length > 0 then
    if token_length > 8 or offset + token_length > length then
      subtree:add_expert_info(PI_MALFORMED, PI_ERROR,
        string.format("Invalid Token Length %d", token_length))
      return length
    end
    subtree:add(f.token, buffer(offset, token_length))
    offset = offset + token_length
  end

  local last_opt = 0

  if offset < length and buffer(offset,1):uint() ~= 0xFF then
    local opts_t = subtree:add(spacecoap_proto, buffer(offset), "Options")

    while offset < length do
      local hdr = buffer(offset,1):uint()
      if hdr == 0xFF then break end
      offset = offset + 1

      local opt_delta = math.floor(hdr / 16)
      local opt_len   = hdr % 16

      if opt_delta == 13 then
        opt_delta = buffer(offset,1):uint() + 13; offset = offset + 1
      elseif opt_delta == 14 then
        opt_delta = buffer(offset,2):uint() + 269; offset = offset + 2
      elseif opt_delta == 15 then
        opts_t:add_expert_info(PI_PROTOCOL, PI_ERROR,
          "Invalid option delta=15")
        opt_delta = 0
      end

      if opt_len == 13 then
        opt_len = buffer(offset,1):uint() + 13; offset = offset + 1
      elseif opt_len == 14 then
        opt_len = buffer(offset,2):uint() + 269; offset = offset + 2
      elseif opt_len == 15 then
        opts_t:add_expert_info(PI_PROTOCOL, PI_ERROR,
          "Invalid option length=15")
        opt_len = 0
      end

      local opt_num = last_opt + opt_delta
      last_opt = opt_num

      if offset + opt_len > length then
        opts_t:add_expert_info(PI_MALFORMED, PI_ERROR,
          "Option value truncated")
        opt_len = length - offset
      end

      local val = buffer(offset, opt_len)
      offset = offset + opt_len

      if     opt_num == 1  then opts_t:add(f.opt_if_match,       val)
      elseif opt_num == 3  then opts_t:add(f.opt_uri_host,       val)
      elseif opt_num == 4  then opts_t:add(f.opt_etag,           val)
      elseif opt_num == 5  then opts_t:add(f.opt_if_none_match,  val)
      elseif opt_num == 6  then opts_t:add(f.opt_observe,        val)
      elseif opt_num == 7  then opts_t:add(f.opt_uri_port,       val)
      elseif opt_num == 8  then opts_t:add(f.opt_location_path,  val)
      elseif opt_num == 11 then opts_t:add(f.opt_uri_path,       val)
      elseif opt_num == 12 then
        local v = val:uint()
        local txt = content_format_names[v] or ("Unknown("..v..")")
        opts_t:add(f.opt_content_format, val, txt.." ("..v..")")
      elseif opt_num == 14 then opts_t:add(f.opt_max_age,        val)
      elseif opt_num == 15 then opts_t:add(f.opt_uri_query,      val)
      elseif opt_num == 17 then opts_t:add(f.opt_accept,         val)
      elseif opt_num == 20 then opts_t:add(f.opt_location_query, val)
      elseif opt_num == 23 then
        local raw = val:uint()
        local blk_num = math.floor(raw / 16)
        local rem4    = raw % 16
        local m       = math.floor(rem4 / 8)
        local szx     = rem4 % 8
        local sz      = 2 ^ (szx + 4)
        opts_t:add(f.opt_block2, val,
          string.format("Block2: NUM=%d, M=%d, SZX=%d (%d bytes)", blk_num, m, szx, sz))
      elseif opt_num == 27 then
        local raw = val:uint()
        local blk_num = math.floor(raw / 16)
        local rem4    = raw % 16
        local m       = math.floor(rem4 / 8)
        local szx     = rem4 % 8
        local sz      = 2 ^ (szx + 4)
        opts_t:add(f.opt_block1, val,
          string.format("Block1: NUM=%d, M=%d, SZX=%d (%d bytes)", blk_num, m, szx, sz))
      elseif opt_num == 28 then opts_t:add(f.opt_size2,       val)
      elseif opt_num == 35 then opts_t:add(f.opt_proxy_uri,    val)
      elseif opt_num == 39 then opts_t:add(f.opt_proxy_scheme, val)
      elseif opt_num == 60 then opts_t:add(f.opt_size1,       val)
      elseif opt_num == 65000 then
        opts_t:add(f.opt_payload_length, val)      
      else
        opts_t:add(val, string.format("Option %d: %s", opt_num, tostring(val)))
      end
    end
  end

  if offset < length and buffer(offset,1):uint() == 0xFF then
    offset = offset + 1
  end
  if offset < length then
    subtree:add(f.payload, buffer(offset))
  end

  return length
end

-- Register protocol for standard CoAP ports
local udp_table = DissectorTable.get("udp.port")
udp_table:add(5683, spacecoap_proto)
udp_table:add(5684, spacecoap_proto)

-- Heuristic dissector to detect SpaceCoAP on unknown ports
local function spacecoap_heuristic(buffer, pinfo, tree)
  if buffer:len() < 5 then return false end
  local b0 = buffer(0,1):uint()
  if math.floor(b0 / 64) ~= 1 then return false end
  local tkl = b0 % 16
  if buffer:len() < 5 + tkl then return false end
  spacecoap_proto.dissector(buffer, pinfo, tree)
  return true
end
spacecoap_proto:register_heuristic("udp", spacecoap_heuristic)
