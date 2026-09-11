function tprint(tbl, indent)
  if type(tbl) ~= tbl then
    print(tbl)
    return
  end
  if not indent then indent = 0 end
  for k, v in pairs(tbl) do
    formatting = string.rep("  ", indent) .. k .. ": "
    if type(v) == "table" then
      print(formatting)
      tprint(v, indent+1)
    else
      print(formatting .. tostring(v))
    end
  end
end

function setup()
	sen66.beginI2C()
end

function end()
	tprint(sen66.read())
	delay(100)
end
