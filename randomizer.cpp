#include "randomizer.h"
#include "ModTable.h"
#include "GameData.h"

/*#define FORMMAPADDR 0x00B0613C

NiTMapBase<unsigned int, TESForm*>* allObjects = (NiTMapBase<unsigned int, TESForm*>*)FORMMAPADDR;
*/

std::map<UInt32, std::vector<TESForm*>> allWeapons;
std::map<UInt32, std::vector<TESForm*>> allClothingAndArmor;
std::map<UInt32, std::vector<TESForm*>> allGenericItems;
std::map<UInt32, std::vector<TESForm*>> allSpellsBySchool;
std::vector<TESForm*> allCreatures;
std::vector<TESForm*> allItems;
std::vector<TESForm*> allSpells;
std::set<TESForm*> allAdded;

std::list<TESObjectREFR*> toRandomize;
std::map<TESObjectREFR*, UInt32> restoreFlags;

std::random_device rd;
std::mt19937 gen(rd());

TESForm* obrnFlag = NULL;

int clothingRanges[CRANGE_MAX];
int weaponRanges[WRANGE_MAX];

int oUseEssentialCreatures = 0;
int oExcludeQuestItems = 1;
int oRandCreatures = 1;
int oAddItems = 1;
int oDeathItems = 1;
int oWorldItems = 1;
int oRandInventory = 1;

bool skipMod[0xFF] = { 0 };
UInt8 randId = 0xFF;

UInt32 rng(UInt32 a, UInt32 b) {
	std::uniform_int_distribution<UInt32> dist(a, b);
	return dist(gen);
}

UInt8 GetModIndexShifted(const std::string& name) {
	UInt8 id = ModTable::Get().GetModIndex(name);
	if (id == 0xFF) {
		return id;
	}
	const ModEntry** activeModList = (*g_dataHandler)->GetActiveModList();
	if (stricmp(activeModList[0]->data->name, "oblivion.esm") && activeModList[id] != NULL && stricmp(activeModList[id]->data->name, name.c_str()) == 0) {
		++id;
	}
	return id;
}

void InitModExcludes() {
	randId = GetModIndexShifted("Randomizer.esp");
	for (int i = 0; i < 0xFF; ++i) {
		skipMod[i] = false;
	}
	if (randId != 0xFF) {
		_MESSAGE("Randomizer.esp's ID is %02X", randId);
		skipMod[randId] = true;
	}
	else {
		_ERROR("Couldn't find Randomizer.esp's mod ID. Make sure that you did not rename the plugin file.");
	}
	FILE* f = fopen("Data/RandomizerSkip.cfg", "r");
	if (f == NULL) {
		return;
	}
	char buf[256] = { 0 };
	for (int i = 0; fscanf(f, "%255[^\n]\n", buf) > 0 /* != EOF*/ && i < 0xFF; ++i) {
		//same reasoning as in InitConfig()
		UInt8 id = GetModIndexShifted(buf);
		if (id == 0xFF) {
			_WARNING("Could not get mod ID for mod %s", buf);
			continue;
		}
		skipMod[id] = true;
		_MESSAGE("Skipping mod %s\n", buf);
	}
	fclose(f);
}

char* cfgeol(char* s) {
	for (char *p = s; *p != 0; ++p) {
		if (*p == ';' || *p == '\t' || *p == '\n' || *p == '\r') {
			return p;
		}
	}
	return NULL;
}

#define OBRN_VAR2STR(x) #x
#define OBRN_PARSECONFIGLINE(line, var, str, len) \
if (strncmp(line, str, len) == 0 && isspace(line[len])) {\
	var = atoi(line + len + 1); \
	continue; \
}

#define OBRN_CONFIGLINE(line, var) \
{\
	int len = strlen(OBRN_VAR2STR(set ZZZOBRNRandomQuest.##var to));\
	OBRN_PARSECONFIGLINE(line, var, OBRN_VAR2STR(set ZZZOBRNRandomQuest.##var to), len);\
}

//this is not an elegant solution but i dont want to split the config into two files
void InitConfig() {
	FILE* f = fopen("Data/Randomizer.cfg", "r");
	if (f == NULL) {
		return;
	}
	char buf[256] = { 0 };
	for (int i = 0; fscanf(f, "%255[^\n]\n", buf) > 0/* != EOF*/ && i < 512; ++i) { 
		//the number of iterations is not necessary given the fscanf > 0 check but I'll sleep better knowing that this will never cause an infinite loop
		if (char* p = cfgeol(buf)) {
			*p = 0;
		}
		OBRN_CONFIGLINE(buf, oUseEssentialCreatures);
		OBRN_CONFIGLINE(buf, oExcludeQuestItems);
		OBRN_CONFIGLINE(buf, oRandCreatures);
		OBRN_CONFIGLINE(buf, oAddItems);
		OBRN_CONFIGLINE(buf, oDeathItems);
		OBRN_CONFIGLINE(buf, oWorldItems);
		OBRN_CONFIGLINE(buf, oRandInventory);
		//set ZZZOBRNRandomQuest.oUseEssentialCreatures to 0
		//set ZZZOBRNRandomQuest.oExcludeQuestItems to 1
	}
	fclose(f);
}

int clothingKeys[] = { kSlot_UpperBody, kSlot_UpperHand, kSlot_UpperLower, kSlot_UpperLowerFoot, kSlot_UpperLowerHandFoot, kSlot_UpperLowerHand, -1 };
void fillUpClothingRanges() {
	for (int i = 0; i < CRANGE_MAX; ++i) {
		clothingRanges[i] = 0;
	}
	for (int* k = &clothingKeys[0], i = -1; *k != -1; ++k) {
		auto it = allClothingAndArmor.find(*k);//allClothingAndArmor.find(TESBipedModelForm::SlotForMask(*k));
		if (it == allClothingAndArmor.end()) {
			continue;
		}
		clothingRanges[++i] = it->second.size();
	}
	for (int i = 1; i < CRANGE_MAX; ++i) {
		clothingRanges[i] += clothingRanges[i - 1];
	}
}

void fillUpWpRanges() {
	int wpKeys[] = { TESObjectWEAP::kType_BladeOneHand, TESObjectWEAP::kType_BluntOneHand, TESObjectWEAP::kType_Staff, TESObjectWEAP::kType_Bow, -1 };
	int maxUnarmed = 0;
	for (int i = 0; i < WRANGE_MAX; ++i) {
		weaponRanges[i] = 0;
	}
	for (int* k = &wpKeys[0], i = -1; *k != -1; ++k) {
		auto it = allWeapons.find(*k);
		if (it == allWeapons.end()) {
			continue;
		}
		weaponRanges[++i] = it->second.size();
	}
	weaponRanges[UNARMED] = weaponRanges[BOWS] * (10.0 / 9.0);
	for (int i = 1; i < WRANGE_MAX; ++i) {
		weaponRanges[i] += weaponRanges[i - 1];
	}
}

TESForm* getRandomForKey(ItemMapPtr map, const UInt32 key) {
	auto it = map->find(key);
	if (it == map->end() || !it->second.size()) {
		_ERROR("Couldn't find key %u for map %s\n", key, (map == &allWeapons ? "weapons" : (map == &allClothingAndArmor ? "clothing & armor" : "generic")));
		return NULL;
	}
	return it->second[rng(0, it->second.size() - 1)];
}

void addOrAppend(ItemMapPtr map, const UInt32 key, TESForm* value) {
	allAdded.insert(value);
	auto it = map->find(key);
	if (it == map->end()) {
		std::vector<TESForm*> itemList;
		itemList.push_back(value);
		map->insert(std::make_pair(key, itemList));
	}
	else {
		it->second.push_back(value);
	}
}

bool isQuestItem(TESForm* item) {
	if (item->IsQuestItem()) {
		return true;
	}
	TESScriptableForm* scriptForm = OBLIVION_CAST(item, TESForm, TESScriptableForm);
	if (scriptForm != NULL && scriptForm->script != NULL) {
		return true;
	}
	return false;
}

bool tryToAddForm(TESForm* f) {
	ItemMapPtr ptr = NULL;
	UInt32 key = 0xFFFFFFFF;
	const char* name = GetFullName(f);
	if (name[0] == '<') {
		return false;
	}
	if (obrnFlag == NULL && f->GetModIndex() == randId && f->GetFormType() == kFormType_Misc && strcmp(name, "You should not see this") == 0) {
		obrnFlag = f;
		_MESSAGE("OBRN Flag found as %08X", f->refID);
		return false;
	}
	if (f->GetModIndex() == 0xFF || skipMod[f->GetModIndex()]) {
		return false;
	}
	if (f->GetFormType() != kFormType_Creature && f->IsQuestItem() && oExcludeQuestItems) {
		return false;
	}
	if (allAdded.find(f) != allAdded.end()) {
		return false;
	}
	if (strncmp(name, "aaa", 3) == 0) {
		//exception for some test objects that typically don't even have a working model
		return false;
	}
	switch (f->GetFormType()) {
	case kFormType_Creature:
	{
		TESCreature* critter = OBLIVION_CAST(f, TESForm, TESCreature);
		TESScriptableForm* scriptForm = OBLIVION_CAST(f, TESForm, TESScriptableForm);
		if (scriptForm != NULL && critter != NULL) {
			if ((oUseEssentialCreatures || (!critter->actorBaseData.IsEssential()) && 
				(!oExcludeQuestItems || scriptForm->script == NULL))) {
				const char* model = critter->modelList.modelList.Info();
				if (model == NULL) {
					model = "";
				}
				//hardcoded exception for SI grummites without a working model + excluding some test creatures
				if (strstr(name, "Test") == NULL && (strncmp(name, "Grummite Whelp", 14) ||
					strncmp(model, "GobLegs01.NIF", 13))) {
					allCreatures.push_back(f);
					allAdded.insert(f);
					return true;
				}
			}
		}
#ifdef DEBUG
		_MESSAGE("Skipping %08X (%s)", f->refID, name);
#endif
		break;
	}
	case kFormType_Armor:
	case kFormType_Clothing:
	{
		ptr = &allClothingAndArmor;
		if (f->GetFormType() == kFormType_Armor) {
			TESObjectARMO* armor = OBLIVION_CAST(f, TESForm, TESObjectARMO);
			key = armor->bipedModel.partMask & ~kSlot_Tail; //yeah... we kinda dont care about this slot
		}
		else {
			TESObjectCLOT* clothing = OBLIVION_CAST(f, TESForm, TESObjectCLOT);
			key = clothing->bipedModel.partMask;
		}
		if (key & kSlot_RightRing) {
			key = kSlot_LeftRing;
		}
		if (key & kSlot_Hair || key & kSlot_Head) {
			key = kSlot_Head;
		}
		break;
	}
	case kFormType_Weapon:
	{
		ptr = &allWeapons;
		TESObjectWEAP* weapon = OBLIVION_CAST(f, TESForm, TESObjectWEAP);
		key = weapon->type;
		if (key == TESObjectWEAP::kType_BladeTwoHand) {
			key = TESObjectWEAP::kType_BladeOneHand;
		}
		else if (key == TESObjectWEAP::kType_BluntTwoHand) {
			key = TESObjectWEAP::kType_BluntOneHand;
		}
		break;
	}
	case kFormType_Apparatus:
	case kFormType_Book:
	case kFormType_Ingredient:
	case kFormType_Misc:
	case kFormType_Ammo:
	case kFormType_SoulGem:
	case kFormType_Key:
	case kFormType_AlchemyItem:
	case kFormType_SigilStone:
		ptr = &allGenericItems;
		key = f->GetFormType();
		break;
	case kFormType_Spell:
	{
		SpellItem* spell = OBLIVION_CAST(f, TESForm, SpellItem);
		ptr = &allSpellsBySchool;
		key = spell->GetSchool();
		allSpells.push_back(f);
		break;
	}
	default:
		break;
	}
	if (ptr != NULL && key != 0xFFFFFFFF) {
		addOrAppend(ptr, key, f);
		return true;
	}
	return false;
}

bool refIsItem(TESObjectREFR* ref) {
	switch (ref->baseForm->GetFormType()) {
	case kFormType_Armor:
	case kFormType_Clothing:
	case kFormType_Weapon:
	case kFormType_Apparatus:
	case kFormType_Book:
	case kFormType_Ingredient:
	case kFormType_Misc:
	case kFormType_Ammo:
	case kFormType_SoulGem:
	case kFormType_Key:
	case kFormType_AlchemyItem:
	case kFormType_SigilStone:
		return true;
	default:
		return false;
	}
}

bool itemIsEquippable(TESForm* item) {
	switch (item->GetFormType()) {
	case kFormType_Armor:
	case kFormType_Clothing:
	case kFormType_Weapon:
	case kFormType_Ammo:
		return true;
	default:
		return false;
	}
}

const char* formToString[] = {
	TESFORM2STRING(kFormType_None),
	TESFORM2STRING(kFormType_TES4),
	TESFORM2STRING(kFormType_Group),
	TESFORM2STRING(kFormType_GMST),
	TESFORM2STRING(kFormType_Global),
	TESFORM2STRING(kFormType_Class),
	TESFORM2STRING(kFormType_Faction),
	TESFORM2STRING(kFormType_Hair),
	TESFORM2STRING(kFormType_Eyes),
	TESFORM2STRING(kFormType_Race),
	TESFORM2STRING(kFormType_Sound),
	TESFORM2STRING(kFormType_Skill),
	TESFORM2STRING(kFormType_Effect),
	TESFORM2STRING(kFormType_Script),
	TESFORM2STRING(kFormType_LandTexture),
	TESFORM2STRING(kFormType_Enchantment),
	TESFORM2STRING(kFormType_Spell),
	TESFORM2STRING(kFormType_BirthSign),
	TESFORM2STRING(kFormType_Activator),
	TESFORM2STRING(kFormType_Apparatus),
	TESFORM2STRING(kFormType_Armor),
	TESFORM2STRING(kFormType_Book),
	TESFORM2STRING(kFormType_Clothing),
	TESFORM2STRING(kFormType_Container),
	TESFORM2STRING(kFormType_Door),
	TESFORM2STRING(kFormType_Ingredient),
	TESFORM2STRING(kFormType_Light),
	TESFORM2STRING(kFormType_Misc),
	TESFORM2STRING(kFormType_Stat),
	TESFORM2STRING(kFormType_Grass),
	TESFORM2STRING(kFormType_Tree),
	TESFORM2STRING(kFormType_Flora),
	TESFORM2STRING(kFormType_Furniture),
	TESFORM2STRING(kFormType_Weapon),
	TESFORM2STRING(kFormType_Ammo),
	TESFORM2STRING(kFormType_NPC),
	TESFORM2STRING(kFormType_Creature),
	TESFORM2STRING(kFormType_LeveledCreature),
	TESFORM2STRING(kFormType_SoulGem),
	TESFORM2STRING(kFormType_Key),
	TESFORM2STRING(kFormType_AlchemyItem),
	TESFORM2STRING(kFormType_SubSpace),
	TESFORM2STRING(kFormType_SigilStone),
	TESFORM2STRING(kFormType_LeveledItem),
	TESFORM2STRING(kFormType_SNDG),
	TESFORM2STRING(kFormType_Weather),
	TESFORM2STRING(kFormType_Climate),
	TESFORM2STRING(kFormType_Region),
	TESFORM2STRING(kFormType_Cell),
	TESFORM2STRING(kFormType_REFR),
	TESFORM2STRING(kFormType_ACHR),
	TESFORM2STRING(kFormType_ACRE),
	TESFORM2STRING(kFormType_PathGrid),
	TESFORM2STRING(kFormType_WorldSpace),
	TESFORM2STRING(kFormType_Land),
	TESFORM2STRING(kFormType_TLOD),
	TESFORM2STRING(kFormType_Road),
	TESFORM2STRING(kFormType_Dialog),
	TESFORM2STRING(kFormType_DialogInfo),
	TESFORM2STRING(kFormType_Quest),
	TESFORM2STRING(kFormType_Idle),
	TESFORM2STRING(kFormType_Package),
	TESFORM2STRING(kFormType_CombatStyle),
	TESFORM2STRING(kFormType_LoadScreen),
	TESFORM2STRING(kFormType_LeveledSpell),
	TESFORM2STRING(kFormType_ANIO),
	TESFORM2STRING(kFormType_WaterForm),
	TESFORM2STRING(kFormType_EffectShader),
	TESFORM2STRING(kFormType_TOFT)
};

const char* FormToString(int form) {
	if (form >= 0 && form < sizeof(formToString) / sizeof(const char*)) {
		return formToString[form];
	}
	return "Unknown";
}

bool getFormsFromLeveledList(TESLevItem* lev, std::map<UInt32, TESForm*>& forms) {
	auto data = lev->leveledList.list.Info();
	auto next = lev->leveledList.list.Next();
	while (data != NULL) {
		if (data->form->GetFormType() == kFormType_LeveledItem) {
			if (!getFormsFromLeveledList(OBLIVION_CAST(data->form, TESForm, TESLevItem), forms)) {
				return false;
			}
		}
		else {
			if (isQuestItem(data->form) && oExcludeQuestItems) {
				return false;
			}
			forms.insert(std::make_pair(data->form->GetFormType(), data->form));
		}
		if (next == NULL) {
			break;
		}
		data = next->Info();
		next = next->Next();
	}
	return true;
}

void getInventoryFromTESLevItem(TESLevItem* lev, std::map<TESForm*, int>& itemList, bool addQuestItems) {
	auto data = lev->leveledList.list.Info();
	auto next = lev->leveledList.list.Next();
	while (data != NULL) {
		if (data->form->GetFormType() == kFormType_LeveledItem) {
			getInventoryFromTESLevItem(OBLIVION_CAST(data->form, TESForm, TESLevItem), itemList, addQuestItems);
		}
		else {
			if (!isQuestItem(data->form) || addQuestItems) {
				auto it = itemList.find(data->form);
				int count = max(1, data->count);
				if (it == itemList.end()) {
					itemList.insert(std::make_pair(data->form, count));
				}
				else {
					it->second += count;
				}
			}
		}
		if (next == NULL) {
			break;
		}
		data = next->Info();
		next = next->Next();
	}
}

std::pair<TESForm*, int> getFormFromTESLevItem(TESLevItem* lev, bool addQuestItems) {
	std::map<TESForm*, int> itemList;
	getInventoryFromTESLevItem(lev, itemList, addQuestItems);
	if (!itemList.size()) {
		return std::make_pair((TESForm*)NULL, (int)0);
	}
	int r = rng(0, itemList.size() - 1), i = -1;
	for (auto it : itemList) {
		if (++i == r) {
#ifdef _DEBUG
			_MESSAGE("%s: we are returning %s %08X from the leveled list", __FUNCTION__, GetFullName(it.first), it.second);
#endif
			return it;
		}
	}
	return std::make_pair((TESForm*)NULL, (int)0);
}

bool getInventoryFromTESContainer(TESContainer* container, std::map<TESForm*, int>& itemList, bool addQuestItems) {
	bool hasFlag = false;
	auto data = container->list.Info();
	auto next = container->list.Next();
	while (data != NULL) {
		if (data->type->GetFormType() == kFormType_LeveledItem) {
			auto it = getFormFromTESLevItem(OBLIVION_CAST(data->type, TESForm, TESLevItem), addQuestItems);
			if (it.first != NULL) {
				itemList.insert(it);
			}
		}
		else {
			if (data->type == obrnFlag) {
#ifdef _DEBUG
				_MESSAGE("%s: OBRN Flag has been found for", __FUNCTION__);
#endif
				hasFlag = true;
			}
			else if (!isQuestItem(data->type) || addQuestItems) {
				auto it = itemList.find(data->type);
				int count = max(1, data->count);
				if (it == itemList.end()) {
					itemList.insert(std::make_pair(data->type, count));
				}
				else {
					it->second += count;
				}
			}
		}
		if (next == NULL) {
			break;
		}
		data = next->Info();
		next = next->Next();
	}
	return hasFlag;
}

bool getContainerInventory(TESObjectREFR* ref, std::map<TESForm*, int> & itemList, bool addQuestItems) {
	bool hasFlag = false;
	ExtraContainerChanges* cont = (ExtraContainerChanges*)ref->baseExtraList.GetByType(kExtraData_ContainerChanges);
	while (cont != NULL && cont->data != NULL && cont->data->objList != NULL) {
		for (auto it = cont->data->objList->Begin(); !it.End(); ++it) {
			TESForm* item = it->type;
			UInt32 count = max(1, it->countDelta); //certain items, for whatever reasons, have count 0
			if (item == obrnFlag) {
				hasFlag = true;
#ifdef _DEBUG
				_MESSAGE("%s: OBRN Flag has been found for %s (%08X)", __FUNCTION__, GetFullName(ref), ref->refID);
#endif
				continue;
			}
			//_MESSAGE("Ref: %s (%08X) (base: %s %08X) : found a %s (%08X %s), quantity: %i",
			//	GetFullName(ref), ref->refID, GetFullName(ref->baseForm), ref->baseForm->refID, GetFullName(item), item->refID, FormToString(item->GetFormType()), count);
			TESScriptableForm* scriptForm = OBLIVION_CAST(item, TESForm, TESScriptableForm);
			if (GetFullName(item)[0] != '<' && (addQuestItems || (!item->IsQuestItem() && (!oExcludeQuestItems || scriptForm == NULL || scriptForm->script == NULL)))) {
				auto it = itemList.find(item);
				if (it == itemList.end()) {
					itemList.insert(std::make_pair(item, count));
				}
				else {
					it->second += count;
				}
			}
		}
		BSExtraData* ptr = cont->next;
		while (ptr != NULL) {// && cont->next->type == kExtraData_ContainerChanges) {
			if (ptr->type == kExtraData_ContainerChanges) {
				break;
			}
			ptr = ptr->next;
		}
		cont = (ExtraContainerChanges*)ptr;
	}
	if (ref->GetFormType() == kFormType_ACRE && !itemList.size()) {
		TESActorBase* actorBase = OBLIVION_CAST(ref->baseForm, TESForm, TESActorBase);
		if (actorBase == NULL) {
			return false;
		}
		return getInventoryFromTESContainer(&actorBase->container, itemList, addQuestItems);
	}
	return hasFlag;
}

int getPlayerLevel() {
	TESActorBase* actorBase = OBLIVION_CAST(*g_thePlayer, PlayerCharacter, TESActorBase);
	if (actorBase == NULL) {
		return 0;
	}
	return actorBase->actorBaseData.level;
}

int getRefLevelAdjusted(TESObjectREFR* ref) {
	if (!ref->IsActor()) {
		return 1;
	}
	Actor* actor = OBLIVION_CAST(ref, TESObjectREFR, Actor);
	TESActorBase* actorBase = OBLIVION_CAST(actor->baseForm, TESForm, TESActorBase);
	return min(1, actorBase->actorBaseData.IsPCLevelOffset() ? getPlayerLevel() + actorBase->actorBaseData.level : actorBase->actorBaseData.level);
}

void randomizeInventory(TESObjectREFR* ref) {
	if (allGenericItems.size() == 0 || allWeapons.size() == 0 || allClothingAndArmor.size() == 0) {
		return;
	}
	std::map<TESForm*, int> removeItems;
	bool hasFlag = getContainerInventory(ref, removeItems, !oExcludeQuestItems);
	if (ref->GetFormType() == kFormType_ACRE) {
		if (obrnFlag == NULL || hasFlag) {
			return;
		}
#ifdef _DEBUG
		_MESSAGE("%s %08X didn't have the flag - adding it now", GetFullName(ref), ref->refID);
#endif
		ref->AddItem(obrnFlag, NULL, 1);
	}
	for (auto it = removeItems.begin(); it != removeItems.end(); ++it) {
		TESForm* item = it->first;
		int cnt = it->second;
		if (oRandInventory == 1) {
			switch (item->refID) {
			case ITEM_GOLD:
				ref->AddItem(item, NULL, rng(5, 60) * getRefLevelAdjusted(ref) - cnt);
				break;
			case ITEM_LOCKPICK:
			case ITEM_REPAIRHAMMER:
				ref->AddItem(item, NULL, rng(1, 4) *  cnt);
				break;
			default:
			{
				if (TESForm* newItem = getRandomByType(item)) {
#ifdef _DEBUG
					_MESSAGE("Replacing item %s %08X with %s %08X x%i", GetFullName(item), item->refID, GetFullName(newItem), newItem->refID, cnt);
#endif
					ref->AddItem(newItem, NULL, cnt);
					if (ref->GetFormType() == kFormType_ACHR && itemIsEquippable(newItem)) {
						ref->Equip(newItem, 1, NULL, 0);
					}
				}
			}
			}
		}
		if (item->refID != ITEM_GOLD && item->GetFormType() != kFormType_Book && item->GetFormType() != kFormType_Key &&
			/*(item->GetModIndex() != 0xFF && !skipMod[item->GetModIndex()])*/ item->GetModIndex() != 0xFF && item->GetModIndex() != randId && //i honestly do not remember why i put the original check here
			(!oExcludeQuestItems || !isQuestItem(item))) {
			ref->RemoveItem(item, NULL, cnt, 0, 0, NULL, NULL, NULL, 0, 0);
		}
	}
	removeItems.clear();
	if (oRandInventory == 1) {
		return;
	}
	//granting random items
	TESForm* selection = NULL;
	if (ref->GetFormType() == kFormType_ACHR) {
		int roll = rand() % 100, level = getRefLevelAdjusted(ref);
		//Gold
		if (roll < 50) {
#ifdef _DEBUG
			_MESSAGE("GOLD: %s receiving gold", GetFullName(ref));
#endif
			ref->AddItem(LookupFormByID(ITEM_GOLD), NULL, rng(5, 60) * level);
		}
		//Weapon Randomization
		int clothingStatus = OBRNRC_LOWER | OBRNRC_FOOT | OBRNRC_HAND | OBRNRC_UPPER;
		bool gotTwoHanded = false;
		bool gotBow = false;
		for (int i = 0; i < 2; ++i) {
			roll = rng(0, weaponRanges[UNARMED]);
			int v;
			for (v = 0; v < WRANGE_MAX; ++v) {
				if (roll < weaponRanges[v]) {
					break;
				}
			}
			switch (v) {
			case BLADES:
				if (selection = getRandomForKey(&allWeapons, TESObjectWEAP::kType_BladeOneHand)) {
					TESObjectWEAP* wp = OBLIVION_CAST(selection, TESForm, TESObjectWEAP);
#ifdef _DEBUG
					_MESSAGE("BLADE: %s receiving %s", GetFullName(ref), GetFullName(wp));
#endif
					ref->AddItem(wp, NULL, 1);
					if (!i) {
						ref->Equip(wp, 1, NULL, 0);
					}
					if (wp->type == TESObjectWEAP::kType_BladeTwoHand) {
						gotTwoHanded = true;
					}
				}
				break;
			case BLUNT:
				if (selection = getRandomForKey(&allWeapons, TESObjectWEAP::kType_BluntOneHand)) {
					TESObjectWEAP* wp = OBLIVION_CAST(selection, TESForm, TESObjectWEAP);
#ifdef _DEBUG
					_MESSAGE("BLUNT: %s receiving %s", GetFullName(ref), GetFullName(wp));
#endif
					ref->AddItem(wp, NULL, 1);
					if (!i) {
						ref->Equip(wp, 1, NULL, 0);
					}
					if (wp->type == TESObjectWEAP::kType_BluntTwoHand) {
						gotTwoHanded = true;
					}
				}
				break;
			case STAVES:
				if (selection = getRandomForKey(&allWeapons, TESObjectWEAP::kType_Staff)) {
					TESObjectWEAP* wp = OBLIVION_CAST(selection, TESForm, TESObjectWEAP);
#ifdef _DEBUG
					_MESSAGE("STAFF: %s receiving %s", GetFullName(ref), GetFullName(wp));
#endif
					ref->AddItem(wp, NULL, 1);
					if (!i) {
						ref->Equip(wp, 1, NULL, 0);
					}
					gotTwoHanded = true;
				}
			case BOWS:
				if (selection = getRandomForKey(&allWeapons, TESObjectWEAP::kType_Bow)) {
					TESObjectWEAP* wp = OBLIVION_CAST(selection, TESForm, TESObjectWEAP);
#ifdef _DEBUG
					_MESSAGE("BOW: %s receiving %s", GetFullName(ref), GetFullName(wp));
#endif
					ref->AddItem(wp, NULL, 1);
					if (!i) {
						ref->Equip(wp, 1, NULL, 0);
					}
					gotTwoHanded = true;
					gotBow = true;
				}
			default:
#ifdef _DEBUG
				_MESSAGE("UNARMED: %s is unarmed", GetFullName(ref));
#endif
				break;
			}
		}
		if (!gotBow) {
			if (!rng(0, 5)) {
				if (selection = getRandomForKey(&allWeapons, TESObjectWEAP::kType_Bow)) {
					TESObjectWEAP* wp = OBLIVION_CAST(selection, TESForm, TESObjectWEAP);
#ifdef _DEBUG
					_MESSAGE("BOW: %s receiving %s", GetFullName(ref), GetFullName(wp));
#endif
					ref->AddItem(wp, NULL, 1);
					ref->Equip(wp, 1, NULL, 0);
					gotTwoHanded = true;
					gotBow = true;
				}
			}
		}
		if (gotBow) {
			//give arrows
			for (int i = 0; i < 10; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Ammo)) {
#ifdef _DEBUG
					_MESSAGE("ARROW: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
					ref->AddItem(selection, NULL, rng(5, 30));
					if (!i) {
						ref->Equip(selection, 1, NULL, 0);
					}
					if (rng(0, i + 2)) {
						break;
					}
				}
			}
		}
		//Shield
		if (!gotTwoHanded && !rng(0, 2) && (selection = getRandomForKey(&allClothingAndArmor, kSlot_Shield))) {
#ifdef _DEBUG
			_MESSAGE("SHIELD: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
			ref->AddItem(selection, NULL, 1);
			ref->Equip(selection, 1, NULL, 0);
		}
		//Clothing
		if (!rng(0, 19)) {
			clothingStatus &= ~OBRNRC_UPPER;
		}
		if (!rng(0, 15)) {
			clothingStatus &= ~OBRNRC_LOWER;
		}
		if (!rng(0, 13)) {
			clothingStatus &= ~OBRNRC_FOOT;
		}
		if (!rng(0, 9)) {
			clothingStatus &= ~OBRNRC_HAND;
		}
		if (clothingStatus & OBRNRC_UPPER) {
			roll = rng(0, clothingRanges[UPPERLOWERHAND]);
			int v;
			for (v = 0; v < CRANGE_MAX; ++v) {
				if (roll < clothingRanges[v]) {
					break;
				}
			}
			switch (v) {
			case UPPER:
				break;
			case UPPERHAND:
				clothingStatus &= ~OBRNRC_HAND;
				break;
			case UPPERLOWER:
				clothingStatus &= ~OBRNRC_LOWER;
				break;
			case UPPERLOWERFOOT:
				clothingStatus &= ~(OBRNRC_LOWER | OBRNRC_FOOT);
				break;
			case UPPERLOWERFOOTHAND:
				clothingStatus &= ~(OBRNRC_LOWER | OBRNRC_FOOT | OBRNRC_HAND);
				break;
			case UPPERLOWERHAND:
				clothingStatus &= ~(OBRNRC_LOWER | OBRNRC_HAND);
				break;
			default:
				break;
			}
			if (v != CRANGE_MAX) {
				if (selection = getRandomForKey(&allClothingAndArmor, clothingKeys[v])) {
#ifdef _DEBUG
					_MESSAGE("UPPER: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
					ref->AddItem(selection, NULL, 1);
					ref->Equip(selection, 1, NULL, 0);
				}
			}
			else {
#ifdef _DEBUG
				_ERROR("v == CRANGE_MAX. this should not happen");
#endif
			}
		}
		if (clothingStatus & OBRNRC_LOWER) {
			if (selection = getRandomForKey(&allClothingAndArmor, kSlot_LowerBody)) {
#ifdef _DEBUG
				_MESSAGE("LOWER: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, 1);
				ref->Equip(selection, 1, NULL, 0);
			}
		}
		if (clothingStatus & OBRNRC_FOOT) {
			if (selection = getRandomForKey(&allClothingAndArmor, kSlot_Foot)) {
#ifdef _DEBUG
				_MESSAGE("FOOT: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, 1);
				ref->Equip(selection, 1, NULL, 0);
			}
		}
		if (clothingStatus & OBRNRC_HAND) {
			if (selection = getRandomForKey(&allClothingAndArmor, kSlot_Hand)) {
#ifdef _DEBUG
				_MESSAGE("HAND: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, 1);
				ref->Equip(selection, 1, NULL, 0);
			}
		}
		//Head
		if (rng(0, 2)) {
			if (selection = getRandomForKey(&allClothingAndArmor, kSlot_Head)) {
#ifdef _DEBUG
				_MESSAGE("HEAD: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, 1);
				ref->Equip(selection, 1, NULL, 0);
			}
		}
		//Rings
		for (int i = 0; i < 2; ++i) {
			if (!rng(0, i + 1)) {
				if (selection = getRandomForKey(&allClothingAndArmor, kSlot_LeftRing)) {
#ifdef _DEBUG
					_MESSAGE("RING: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
					ref->AddItem(selection, NULL, 1);
					ref->Equip(selection, 1, NULL, 0);
				}
			}
		}
		//Amulets
		if (!rng(0, 2)) {
			if (selection = getRandomForKey(&allClothingAndArmor, kSlot_Amulet)) {
#ifdef _DEBUG
				_MESSAGE("AMULET: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, 1);
				ref->Equip(selection, 1, NULL, 0);
			}
		}
		//Potions
		for (int i = 0; i < 5; ++i) {
			if (rng(0, 2)) {
				break;
			}
			if (selection = getRandomForKey(&allGenericItems, kFormType_AlchemyItem)) {
#ifdef _DEBUG
				_MESSAGE("POTION: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, rng(1, 4));
			}
		}
		//Soul Gems
		for (int i = 0; i < 6; ++i) {
			if (rng(0, 4)) {
				break;
			}
			if (selection = getRandomForKey(&allGenericItems, kFormType_SoulGem)) {
#ifdef _DEBUG
				_MESSAGE("SOULGEM: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, rng(1, 2));
			}
		}
		//Sigil Stones
		if (!rng(0, 9)) {
			if (selection = getRandomForKey(&allGenericItems, kFormType_SigilStone)) {
#ifdef _DEBUG
				_MESSAGE("SIGILSTONE: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
				ref->AddItem(selection, NULL, 1);
			}
		}
		//Ingredients
		if (!rng(0, 5)) {
			for (int i = 0; i < 10; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Ingredient)) {
#ifdef _DEBUG
					_MESSAGE("INGREDIENT: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
					ref->AddItem(selection, NULL, rng(1, 6));
				}
				if (rng(0, 3)) {
					break;
				}
			}
		}
		//Apparatus
		if (!rng(0, 9)) {
			for (int i = 0; i < 3; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Apparatus)) {
#ifdef _DEBUG
					_MESSAGE("APPARATUS: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
					ref->AddItem(selection, NULL, rng(1, 2));
				}
				if (rng(0, 2)) {
					break;
				}
			}
		}
		//Clutter
		if (!rng(0, 9)) {
			for (int i = 0; i < 9; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Misc)) {
#ifdef _DEBUG
					_MESSAGE("GENERIC: %s receiving %s", GetFullName(ref), GetFullName(selection));
#endif
					ref->AddItem(selection, NULL, rng(1, 3));
				}
				if (rng(0, i + 1)) {
					break;
				}
			}
		}
	}
	else {//containers
		//Gold
		if (!rng(0, 2)) {
			ref->AddItem(LookupFormByID(ITEM_GOLD), NULL, rng(1, 100) * (!rng(0, 4) ? 30 : 1));
		}
		//Lockpick
		if (!rng(0, 4)) {
			ref->AddItem(LookupFormByID(ITEM_LOCKPICK), NULL, rng(1, 10));
		}
		//Repair Hammer
		if (!rng(0, 4)) {
			ref->AddItem(LookupFormByID(ITEM_REPAIRHAMMER), NULL, rng(1, 3));
		}
		//Clutter
		if (!rng(0, 5)) {
			for (int i = 0; i < 3; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Misc)) {
					ref->AddItem(selection, NULL, rng(1, 5));
					if (rng(0, i + 3)) {
						break;
					}
				}
			}
		}
		//Potions
		if (!rng(0, 3)) {
			for (int i = 0; i < 5; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_AlchemyItem)) {
					ref->AddItem(selection, NULL, rng(1, 5));
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Ingredients
		if (!rng(0, 4)) {
			for (int i = 0; i < 6; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Ingredient)) {
					ref->AddItem(selection, NULL, rng(1, 2));
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Books
		if (!rng(0, 4)) {
			for (int i = 0; i < 3; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Book)) {
					ref->AddItem(selection, NULL, 1);
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Soul Gems
		if (!rng(0, 9)) {
			for (int i = 0; i < 3; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_SoulGem)) {
					ref->AddItem(selection, NULL, rng(1, 3));
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Sigil Stones
		if (!rng(0, 11)) {
			for (int i = 0; i < 2; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_SigilStone)) {
					ref->AddItem(selection, NULL, 1);
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Keys
		if (!rng(0, 6)) {
			if (selection = getRandomForKey(&allGenericItems, kFormType_Key)) {
				ref->AddItem(selection, NULL, 1);
			}
		}
		//Apparatus
		if (!rng(0, 8)) {
			for (int i = 0; i < 4; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Apparatus)) {
					ref->AddItem(selection, NULL, 1);
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Weapons
		if (!rng(0, 4)) {
			for (int i = 0; i < 3; ++i) {
				int wpType = -1;
				switch (rng(0, 3)) {
				case 0:
					wpType = TESObjectWEAP::kType_BladeOneHand;
					break;
				case 1:
					wpType = TESObjectWEAP::kType_BluntOneHand;
					break;
				case 2:
					wpType = TESObjectWEAP::kType_Bow;
					break;
				default:
					wpType = TESObjectWEAP::kType_Staff;
					break;
				}
				if (selection = getRandomForKey(&allWeapons, wpType)) {
					ref->AddItem(selection, NULL, 1);
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Ammo
		if (!rng(0, 4)) {
			for (int i = 0; i < 4; ++i) {
				if (selection = getRandomForKey(&allGenericItems, kFormType_Ammo)) {
					ref->AddItem(selection, NULL, rng(1, 30));
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
		//Clothing / Armor
		if (!rng(0, 2)) {
			for (int i = 0; i < 7; ++i) {
				UInt32 key = 0xFFFFFFFF;
				switch (rng(0, 12)) { //was 0, 7 in the script
				case 0:
					key = kSlot_Amulet;
					break;
				case 1:
					key = kSlot_Foot;
					break;
				case 2:
					key = kSlot_Hand;
					break;
				case 3:
					key = kSlot_Head;
					break;
				case 4:
					key = kSlot_LowerBody;
					break;
				case 5:
					key = kSlot_LeftRing;
					break;
				case 6:
					key = kSlot_Shield;
					break;
				case 7:
					key = kSlot_UpperBody;
					break;
				case 8:
					key = kSlot_UpperHand;
					break;
				case 9:
					key = kSlot_UpperLower;
					break;
				case 10:
					key = kSlot_UpperLowerFoot;
					break;
				case 11:
					key = kSlot_UpperLowerHandFoot;
					break;
				default:
					key = kSlot_UpperLowerHand;
					break;
				}
				if (key != 0xFFFFFFFF && (selection = getRandomForKey(&allClothingAndArmor, key))) {
					ref->AddItem(selection, NULL, 1);
					if (rng(0, i + 1)) {
						break;
					}
				}
			}
		}
	}
}

TESForm* getFormFromLeveledList(TESLevItem* lev) {
	if (lev == NULL) {
		return NULL;
	}
	std::map<UInt32, TESForm*> forms;
	if (!getFormsFromLeveledList(lev, forms) || !forms.size()) {
		return NULL;
	}
	int i = -1, cnt = rng(0, forms.size() - 1);
	auto it = forms.begin();
	while (it != forms.end()) {
		if (++i == cnt) {
			return it->second;
		}
		++it;
	}
	return NULL;
}

TESForm* getRandom(TESForm* f) {
	if (!allItems.size()) {
		return NULL;
	}
	if (f == NULL) {
		return NULL;
	}
	if (oExcludeQuestItems && isQuestItem(f)) {
		return NULL;
	}
	if (f->GetModIndex() != 0xFF && f->GetModIndex() == randId) {
		//if (f->GetModIndex() != 0xFF && skipMod[f->GetModIndex()]) { //very important!
		return NULL;
	}
	return allItems[rand() % allItems.size()];;
}

TESForm* getRandomByType(TESForm *f) {
	ItemMapPtr ptr = NULL;
	UInt32 key = 0xFFFFFFFF;
	if (f == NULL) {
		return NULL;
	}
	if (oExcludeQuestItems && isQuestItem(f)) {
		return NULL;
	}
	if (f->GetModIndex() != 0xFF && f->GetModIndex() == randId) {
	//if (f->GetModIndex() != 0xFF && skipMod[f->GetModIndex()]) { //very important!
		return NULL;
	}
	switch (f->GetFormType()) {
	case kFormType_LeveledItem:
	{
		TESLevItem* lev = OBLIVION_CAST(f, TESForm, TESLevItem);
		return getRandomByType(getFormFromLeveledList(lev));
	}
	case kFormType_Armor:
	case kFormType_Clothing:
	{
		ptr = &allClothingAndArmor;
		if (f->GetFormType() == kFormType_Armor) {
			TESObjectARMO* armor = OBLIVION_CAST(f, TESForm, TESObjectARMO);
			key = armor->bipedModel.partMask & ~kSlot_Tail; //yeah... we kinda dont care about this slot
		}
		else {
			TESObjectCLOT* clothing = OBLIVION_CAST(f, TESForm, TESObjectCLOT);
			key = clothing->bipedModel.partMask;
		}
		if (key & kSlot_RightRing) {
			key = kSlot_LeftRing;
		}
		if (key & kSlot_Hair || key & kSlot_Head) {
			key = kSlot_Head;
		}
		break;
	}
	case kFormType_Weapon:
	{
		ptr = &allWeapons;
		TESObjectWEAP* weapon = OBLIVION_CAST(f, TESForm, TESObjectWEAP);
		key = weapon->type;
		if (key == TESObjectWEAP::kType_BladeTwoHand) {
			key = TESObjectWEAP::kType_BladeOneHand;
		}
		else if (key == TESObjectWEAP::kType_BluntTwoHand) {
			key = TESObjectWEAP::kType_BluntOneHand;
		}
		break;
	}
	case kFormType_Apparatus:
	case kFormType_Book:
	case kFormType_Ingredient:
	case kFormType_Misc:
	case kFormType_Ammo:
	case kFormType_SoulGem:
	case kFormType_Key:
	case kFormType_AlchemyItem:
	case kFormType_SigilStone:
		ptr = &allGenericItems;
		key = f->GetFormType();
	default:
		break;
	}
	if (ptr != NULL && key != 0xFFFFFFFF) {
		return getRandomForKey(ptr, key);
	}
	return NULL;
}

TESForm* getRandomBySetting(TESForm* f, int option) {
	switch (option) {
	case 0:
		return NULL;
	case 1:
		return getRandomByType(f);
	case 2:
		return getRandom(f);
	default:
		_MESSAGE("Invalid option %i for getRandomBySetting", option);
		return NULL;
	}
}

void randomize(TESObjectREFR* ref, const char* function) {
#ifdef _DEBUG
	_MESSAGE("%s: Attempting to randomize %s %08X", function, GetFullName(ref), ref->refID);
#endif
	if (ref->GetFormType() == kFormType_ACRE) {
		if (allCreatures.size() == 0) {
			return;
		}
		if (strcmp(function, "ESP") == 0) {
			QueueUIMessage_2("Randomizing creatures through the spell may cause issues", 1000, NULL, NULL);
		}
		else if (!oRandCreatures) {
			return;
		}
		Actor* actor = OBLIVION_CAST(ref, TESObjectREFR, Actor);
		if (actor == NULL) {
			return;
		}
		UInt32 health = actor->GetBaseActorValue(kActorVal_Health), aggression = actor->GetActorValue(kActorVal_Aggression);//actor->GetBaseActorValue(kActorVal_Aggression);
		if (health == 0) {
			if (loading_game) {
				toRandomize.push_back(ref);
				return;
			}
#ifdef _DEBUG
			_MESSAGE("%s: Dead creature %s %08X will be treated as a container", function, GetFullName(ref), ref->refID);
#endif
			randomizeInventory(ref);
			return;
		}
		std::map<TESForm*, int> keepItems;
		getContainerInventory(ref, keepItems, true);
		TESForm* oldBaseForm = ref->GetTemplateForm() != NULL ? ref->GetTemplateForm() : ref->baseForm,
			* rando = allCreatures[rng(0, allCreatures.size())];
		TESCreature* creature = OBLIVION_CAST(rando, TESForm, TESCreature);
		if (creature != NULL) {
			getInventoryFromTESContainer(&creature->container, keepItems, true);
		}
#ifdef _DEBUG
		_MESSAGE("%s: Going to randomize %s %08X into %s %08X", function, GetFullName(ref), ref->refID, GetFullName(rando), rando->refID);
#endif
		//TESActorBase* actorBase = OBLIVION_CAST(rando, TESForm, TESActorBase);
		ref->baseForm = rando;
		ref->SetTemplateForm(oldBaseForm);
		actor->SetActorValue(kActorVal_Aggression, aggression);
		for (auto it = keepItems.begin(); it != keepItems.end(); ++it) {
			TESForm* item = it->first;
			int cnt = it->second;
			ref->AddItem(item, NULL, cnt);
		}
	}
	else if (refIsItem(ref)) {
		if (!oWorldItems) {
			return;
		}
#ifdef _DEBUG
		_MESSAGE("%s: World item randomization: will try to randomize %s %08X", function, GetFullName(ref), ref->refID);
#endif
		if (TESForm* rando = getRandomBySetting(ref->baseForm, oWorldItems)) {
#ifdef _DEBUG
			_MESSAGE("%s: Going to randomize %s %08X into %s %08X", function, GetFullName(ref), ref->refID, GetFullName(rando), rando->refID);
#endif
			ref->baseForm = rando;
			ref->Update3D();
		}
	}
	else {
		randomizeInventory(ref);
	}
}

void debugDumpSpells(TESForm* form) {
	TESActorBase* actor_base = OBLIVION_CAST(form, TESForm, TESActorBase);
	if (actor_base != NULL) {
		FILE* f = fopen("npc_spells.txt", "a");
		fprintf(f, "Actor/Creature: %s (%08X)\n", GetFullName(form), form->refID);
		{
			auto data = actor_base->spellList.spellList.Info();
			auto next = actor_base->spellList.spellList.Next();
			while (data != NULL) {
				UInt32 refID = files_read ? data->refID : (UInt32)data;
				TESForm* item = LookupFormByID(refID);
				if (item) {
					fprintf(f, "\tNon-leveled Spell data %08X %s (%08X)\n", data, GetFullName(item), item->refID);
				}
				if (next == NULL) {
					break;
				}
				data = next->Info();
				next = next->Next();
			}
		}
		if (1)
		{
			auto data = actor_base->spellList.leveledSpellList.Info();
			auto next = actor_base->spellList.leveledSpellList.Next();
			while (data != NULL) {
				UInt32 refID = (UInt32)data;
				fprintf(f, "\tLeveled Spell: %s (%08X) %s\n", GetFullName(data), data->refID, FormToString(data->GetFormType()));
				if (next == NULL) {
					break;
				}
				data = next->Info();
				next = next->Next();
			}
		}
		fclose(f);
	}
}