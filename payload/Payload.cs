using UnityEngine;
using System.Runtime.CompilerServices;

namespace Payload
{
    /// <summary>
    /// Bridge between C# and C++.
    /// Each extern method maps to a C++ function registered via mono_add_internal_call.
    /// </summary>
    public static class Bridge
    {
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnGUICallback();

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnUpdateCallback();

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnLateUpdateCallback();
    }

    /// <summary>
    /// Entry point called from C++ after the assembly is loaded.
    /// Creates a persistent GameObject with the overlay MonoBehaviour.
    /// </summary>
    public static class Loader
    {
        public static void Init()
        {
            GameObject go = new GameObject("__SSJJ_Overlay__");
            Object.DontDestroyOnLoad(go);
            go.AddComponent<OverlayBehaviour>();
        }
    }

    /// <summary>
    /// Persistent MonoBehaviour that relays Unity callbacks to C++.
    /// OnGUI      -- called during the IMGUI rendering pass (safe for GUI.*/GUILayout.*).
    /// Update     -- called once per frame (safe for input polling, game-logic reads).
    /// LateUpdate -- called after all Update functions (safe for overriding game state).
    /// </summary>
    public class OverlayBehaviour : MonoBehaviour
    {
        void OnGUI()
        {
            try
            {
                Bridge.OnGUICallback();
            }
            catch (System.Exception) { }
        }

        void Update()
        {
            try
            {
                Bridge.OnUpdateCallback();
            }
            catch (System.Exception) { }
        }

        void LateUpdate()
        {
            try
            {
                Bridge.OnLateUpdateCallback();
            }
            catch (System.Exception) { }
        }
    }
}
