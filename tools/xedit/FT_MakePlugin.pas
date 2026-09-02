{
  FollowerTactics -- create the UseMagic package pool.

  WHY EIGHT, AND NOT ONE
  The spell being cast is stored IN the package record (input "Spell", uid 3 in
  the UseMagic template), not on the actor. One shared record would mean two
  followers casting different spells fight over one field, and whichever wrote
  last decides what BOTH cast. One record per follower slot removes that, and
  eight matches the follower cap the engine already enforces.

  WHY COPY A VANILLA PACKAGE INSTEAD OF BUILDING ONE
  A templated package stores its inputs as UID-tagged slots whose meaning comes
  from the template's name map. Hand-building that is easy to get subtly wrong,
  and the failure mode is a plugin that loads cleanly and does nothing. Copying
  a package the game ships guarantees the shape; we only repoint values.

  WHY THE PATHS ARE DISCOVERED, NOT WRITTEN DOWN
  A first attempt set 'PTDA - Target' directly and died with "is not editable":
  PTDA is a struct and the form reference lives inside it. Rather than guess
  again at the child's name, this walks the element tree and finds the leaf that
  currently holds a form, then writes that. It reports which path it used, so
  the next person does not have to repeat the discovery.

  The report is saved AS IT GOES. The first run aborted before writing anything,
  which turned a five-minute experiment into no information at all.
}
unit UserScript;

const
  REPORT      = 'C:\project\SkyrimFollowerTactics\SSEEdit\ft_makeplugin.txt';
  PLUGIN      = 'FollowerTactics.esp';
  SOURCE      = 'MG07AncanoCastAtEye';
  SLOTS       = 8;
  PLACEHOLDER = $0002F3B8;   // Fast Healing; the C++ repoints this at runtime

  IDX_SPELL       = 3;
  IDX_NUMCAST_MIN = 10;
  IDX_NUMCAST_MAX = 11;

var
  sl: TStringList;

procedure Log(s: string);
begin
  sl.Add(s);
  sl.SaveToFile(REPORT);   // survive an abort
end;

procedure Dump(r: IInterface; indent: string; depth: integer);
var
  i: integer;
  e: IInterface;
begin
  if depth > 6 then Exit;
  for i := 0 to Pred(ElementCount(r)) do begin
    e := ElementByIndex(r, i);
    sl.Add(indent + '<' + Name(e) + '> = [' + GetEditValue(e) + ']');
    if ElementCount(e) > 0 then Dump(e, indent + '   ', depth + 1);
  end;
  sl.SaveToFile(REPORT);
end;

function PositionOfInput(r: IInterface; uid: integer): integer;
var
  inputs, e: IInterface;
  i: integer;
begin
  Result := -1;
  inputs := ElementByPath(r, 'Package Data\Data Inputs');
  if not Assigned(inputs) then Exit;
  for i := 0 to Pred(ElementCount(inputs)) do begin
    e := ElementByIndex(inputs, i);
    if GetElementNativeValues(e, 'UNAM - Index') = uid then begin
      Result := i;
      Exit;
    end;
  end;
end;

function ValueAt(r: IInterface; pos: integer): IInterface;
var
  values: IInterface;
begin
  Result := nil;
  values := ElementByPath(r, 'Package Data\Data Input Values');
  if Assigned(values) and (pos >= 0) and (pos < ElementCount(values)) then
    Result := ElementByIndex(values, pos);
end;

// The deepest child that currently holds a form reference. That is the leaf we
// have to write; its parents are structs and refuse assignment.
function FindFormLeaf(e: IInterface; depth: integer; var path: string): IInterface;
var
  i: integer;
  c, found: IInterface;
  v, sub: string;
begin
  Result := nil;
  if depth > 5 then Exit;
  for i := 0 to Pred(ElementCount(e)) do begin
    c := ElementByIndex(e, i);
    v := GetEditValue(c);
    if (ElementCount(c) = 0) and ((Pos('[SPEL', v) > 0) or (Pos('[PACK', v) > 0)
        or (Pos('[NPC_', v) > 0) or (Pos('[REFR', v) > 0) or (Pos('[ACHR', v) > 0)) then begin
      path := path + '\' + Name(c);
      Result := c;
      Exit;
    end;
    if ElementCount(c) > 0 then begin
      sub := path + '\' + Name(c);
      found := FindFormLeaf(c, depth + 1, sub);
      if Assigned(found) then begin
        path := sub;
        Result := found;
        Exit;
      end;
    end;
  end;
end;

// The scalar child of a value entry.
//
// CNAM is a UNION, not a value: its concrete member depends on the input's
// ANAM type, so assigning to CNAM itself fails with "can not be edited". The
// leaf underneath is what takes the write -- exactly the same shape as the
// Spell input, where the form lives under PTDA\Target Data\Target.
function FindScalarLeaf(e: IInterface; var path: string): IInterface;
var
  i: integer;
  c, inner: IInterface;
  sub: string;
begin
  Result := nil;
  for i := 0 to Pred(ElementCount(e)) do begin
    c := ElementByIndex(e, i);
    if Pos('CNAM', Name(c)) <> 1 then Continue;

    path := path + '\' + Name(c);
    if ElementCount(c) = 0 then begin
      Result := c;
      Exit;
    end;
    // descend to the first leaf of the union
    inner := ElementByIndex(c, 0);
    while Assigned(inner) and (ElementCount(inner) > 0) do begin
      path := path + '\' + Name(inner);
      inner := ElementByIndex(inner, 0);
    end;
    if Assigned(inner) then path := path + '\' + Name(inner);
    Result := inner;
    Exit;
  end;
end;

function Initialize: integer;
var
  i, pos: integer;
  f, g, src, plug, rec, v, leaf: IInterface;
  path: string;
begin
  sl := TStringList.Create;
  Log('FollowerTactics -- plugin creation');
  Log('');

  src := nil;
  for i := 0 to Pred(FileCount) do begin
    f := FileByIndex(i);
    if GetFileName(f) <> 'Skyrim.esm' then Continue;
    g := GroupBySignature(f, 'PACK');
    src := MainRecordByEditorID(g, SOURCE);
  end;

  if not Assigned(src) then begin
    Log('FAILED: source package not found: ' + SOURCE);
    sl.Free; Result := 1; Exit;
  end;
  Log('source: ' + EditorID(src));

  // The structure of the Spell input, in full, so the path is documented even
  // if every write below fails.
  pos := PositionOfInput(src, IDX_SPELL);
  Log('Spell input is at array position ' + IntToStr(pos));
  v := ValueAt(src, pos);
  if Assigned(v) then begin
    Log('---- Spell input structure ----');
    Dump(v, '  ', 0);
    Log('-------------------------------');
  end;
  Log('');

  plug := AddNewFileName(PLUGIN);
  if not Assigned(plug) then begin
    Log('FAILED: AddNewFileName returned nothing -- does ' + PLUGIN + ' already exist?');
    sl.Free; Result := 1; Exit;
  end;
  AddMasterIfMissing(plug, 'Skyrim.esm');
  Log('created ' + PLUGIN);

  for i := 1 to SLOTS do begin
    rec := wbCopyElementToFile(src, plug, True, True);
    SetElementEditValues(rec, 'EDID', 'FT_CastSlot' + IntToStr(i));

    pos := PositionOfInput(rec, IDX_SPELL);
    v := ValueAt(rec, pos);
    if Assigned(v) then begin
      path := '';
      leaf := FindFormLeaf(v, 0, path);
      if Assigned(leaf) then begin
        SetNativeValue(leaf, PLACEHOLDER);
        if i = 1 then Log('  spell written via' + path + '  -> ' + GetEditValue(leaf));
      end else if i = 1 then
        Log('  WARNING: no form leaf found under the Spell input');
    end;

    pos := PositionOfInput(rec, IDX_NUMCAST_MIN);
    v := ValueAt(rec, pos);
    if Assigned(v) then begin
      if i = 1 then begin
        Log('---- NumToCastMin input structure ----');
        Dump(v, '  ', 0);
        Log('--------------------------------------');
      end;
      path := '';
      leaf := FindScalarLeaf(v, path);
      if Assigned(leaf) then begin
        SetNativeValue(leaf, 1);
        if i = 1 then Log('  NumToCastMin written via' + path + ' -> ' + GetEditValue(leaf));
      end else if i = 1 then
        Log('  WARNING: no scalar leaf under NumToCastMin');
    end;

    pos := PositionOfInput(rec, IDX_NUMCAST_MAX);
    v := ValueAt(rec, pos);
    if Assigned(v) then begin
      path := '';
      leaf := FindScalarLeaf(v, path);
      if Assigned(leaf) then SetNativeValue(leaf, 1);
    end;
  end;

  SetIsESL(plug, True);
  Log('');
  Log('created ' + IntToStr(SLOTS) + ' packages (FT_CastSlot1..8), ESL flag set');
  Log('If FollowerTactics.esp is missing from Data, the save step did not run.');

  sl.Free;
  Result := 1;
end;

end.
