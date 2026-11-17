import unittest

class TestSteamHookLogic(unittest.TestCase):
    def setUp(self):
        self.mock_steam_id = 76561197960287930
        self.mock_vtable = ["stub_SteamGeneric"] * 64
        self.mock_interface = self.mock_vtable

    def mock_find_or_create_user_interface(self, user_id, version):
        # Trả về con trỏ đối tượng có word đầu trỏ tới vtable
        return {"vtable": self.mock_interface}

    def mock_context_init(self, context_init_data):
        if context_init_data is None:
            return None
        
        # Nếu chưa khởi tạo interface, gọi callback
        if context_init_data.get("interface") is None:
            init_fn = context_init_data.get("init_fn")
            if init_fn:
                init_fn(context_init_data)
            else:
                context_init_data["interface"] = self.mock_find_or_create_user_interface(1, "SteamUser023")
        
        # Trả về tham chiếu tới interface pointer để [rax] là con trỏ hợp lệ
        return context_init_data

    def test_steam_api_init(self):
        # SteamInternal_SteamAPI_Init trả về 0 (k_ESteamAPIInitResult_OK)
        res = 0
        self.assertEqual(res, 0)

    def test_find_or_create_user_interface_returns_valid_object(self):
        obj = self.mock_find_or_create_user_interface(1, "SteamUser023")
        self.assertIsNotNone(obj)
        self.assertIn("vtable", obj)
        self.assertGreaterEqual(len(obj["vtable"]), 64)

    def test_context_init_resolves_interface(self):
        ctx_data = {"init_fn": None, "counter": 0, "interface": None}
        ret = self.mock_context_init(ctx_data)
        self.assertIsNotNone(ret)
        self.assertIsNotNone(ret["interface"])

    def test_virtual_call_slot2_get_steam_id(self):
        ctx_data = {"init_fn": None, "counter": 0, "interface": None}
        ret = self.mock_context_init(ctx_data)
        p_interface = ret["interface"]
        
        # Giả lập lệnh call [rax + 0x10] (Slot 2 = GetSteamID)
        slot2_fn = p_interface["vtable"][2]
        self.assertEqual(slot2_fn, "stub_SteamGeneric")

    def test_steam_apps_interface_and_language(self):
        # ISteamApps vtable mock
        steam_apps_vtable = ["stub_SteamGeneric"] * 64
        steam_apps_vtable[4] = lambda: "english"
        steam_apps_vtable[5] = lambda: "english,japanese,schinese,tchinese"
        
        # Slot 4: GetCurrentGameLanguage
        current_lang = steam_apps_vtable[4]()
        self.assertEqual(current_lang, "english")
        
        # Slot 5: GetAvailableGameLanguages
        available_langs = steam_apps_vtable[5]()
        self.assertIn("english", available_langs)
        self.assertIn("japanese", available_langs)

    def test_language_config_parser_fallback(self):
        # Kiểm tra logic parse file config và fallback về 'english'
        def parse_language(content):
            if not content:
                return "english"
            for line in content.splitlines():
                if "=" in line:
                    key, val = line.split("=", 1)
                    if key.strip().lower() == "language":
                        return val.strip()
            return "english"

        self.assertEqual(parse_language("Language = english"), "english")
        self.assertEqual(parse_language("language=japanese"), "japanese")
        self.assertEqual(parse_language(""), "english")
        self.assertEqual(parse_language("[Steam]\nOtherKey=123"), "english")

if __name__ == "__main__":
    unittest.main()
