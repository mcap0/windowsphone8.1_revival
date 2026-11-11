// 
// This is the source code of Telegram for Windows Phone v. 3.x.x.
// It is licensed under GNU GPL v. 2 or later.
// You should have received a copy of the license in this archive (see LICENSE).
// 
// Copyright Evgeny Nadymov, 2013-present.
// 
// Minimal replacements for the Blend (Microsoft.Expression.Interactions) types used by the XAML views,
// because that assembly is not available for Windows Phone 8.1. Same names and namespaces, so the XAML only
// drops its "assembly=Microsoft.Expression.Interactions" part. Supported: DataStateBehavior, DataTrigger,
// ChangePropertyAction, ControlStoryboardAction (Play and Stop only).
using System;
using System.Globalization;
using System.Reflection;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Interactivity;

namespace Microsoft.Expression.Interactivity.Core
{
    public class DataStateBehavior : Behavior<FrameworkElement>
    {
        public static readonly DependencyProperty BindingProperty = DependencyProperty.Register("Binding", typeof(object), typeof(DataStateBehavior), new PropertyMetadata(null, OnInputChanged));

        public static readonly DependencyProperty ValueProperty = DependencyProperty.Register("Value", typeof(object), typeof(DataStateBehavior), new PropertyMetadata(null, OnInputChanged));

        public object Binding
        {
            get { return GetValue(BindingProperty); }
            set { SetValue(BindingProperty, value); }
        }

        public object Value
        {
            get { return GetValue(ValueProperty); }
            set { SetValue(ValueProperty, value); }
        }

        public string TrueState { get; set; }

        public string FalseState { get; set; }

        private static void OnInputChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            ((DataStateBehavior)d).Update();
        }

        protected override void OnAttached()
        {
            base.OnAttached();
            Update();
        }

        private void Update()
        {
            if (AssociatedObject == null) return;

            var equal = string.Equals(Convert.ToString(Binding, CultureInfo.InvariantCulture), Convert.ToString(Value, CultureInfo.InvariantCulture), StringComparison.OrdinalIgnoreCase);
            var state = equal ? TrueState : FalseState;
            if (string.IsNullOrEmpty(state)) return;

            DependencyObject current = AssociatedObject;
            while (current != null && !(current is Control))
            {
                current = VisualTreeHelper.GetParent(current);
            }
            var control = current as Control;
            if (control != null)
            {
                VisualStateManager.GoToState(control, state, true);
            }
        }
    }

    public enum ComparisonConditionType
    {
        Equal,
        NotEqual,
        LessThan,
        LessThanOrEqual,
        GreaterThan,
        GreaterThanOrEqual
    }

    public class DataTrigger : TriggerBase<DependencyObject>
    {
        public static readonly DependencyProperty BindingProperty = DependencyProperty.Register("Binding", typeof(object), typeof(DataTrigger), new PropertyMetadata(null, OnInputChanged));

        public static readonly DependencyProperty ValueProperty = DependencyProperty.Register("Value", typeof(object), typeof(DataTrigger), new PropertyMetadata(null, OnInputChanged));

        public object Binding
        {
            get { return GetValue(BindingProperty); }
            set { SetValue(BindingProperty, value); }
        }

        public object Value
        {
            get { return GetValue(ValueProperty); }
            set { SetValue(ValueProperty, value); }
        }

        public ComparisonConditionType Comparison { get; set; }

        private bool _lastResult;

        private static void OnInputChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        {
            ((DataTrigger)d).Evaluate();
        }

        protected override void OnAttached()
        {
            base.OnAttached();
            Evaluate();
        }

        private void Evaluate()
        {
            if (AssociatedObject == null) return;

            var result = Compare(Binding, Value, Comparison);
            if (result && !_lastResult)
            {
                InvokeActions(null);
            }
            _lastResult = result;
        }

        private static bool Compare(object left, object right, ComparisonConditionType comparison)
        {
            double l, r;
            if (double.TryParse(Convert.ToString(left, CultureInfo.InvariantCulture), NumberStyles.Float, CultureInfo.InvariantCulture, out l)
                && double.TryParse(Convert.ToString(right, CultureInfo.InvariantCulture), NumberStyles.Float, CultureInfo.InvariantCulture, out r))
            {
                switch (comparison)
                {
                    case ComparisonConditionType.NotEqual: return l != r;
                    case ComparisonConditionType.LessThan: return l < r;
                    case ComparisonConditionType.LessThanOrEqual: return l <= r;
                    case ComparisonConditionType.GreaterThan: return l > r;
                    case ComparisonConditionType.GreaterThanOrEqual: return l >= r;
                    default: return l == r;
                }
            }

            var equal = string.Equals(Convert.ToString(left, CultureInfo.InvariantCulture), Convert.ToString(right, CultureInfo.InvariantCulture), StringComparison.OrdinalIgnoreCase);
            return comparison == ComparisonConditionType.NotEqual ? !equal : equal;
        }
    }

    public class ChangePropertyAction : TriggerAction<DependencyObject>
    {
        public static readonly DependencyProperty ValueProperty = DependencyProperty.Register("Value", typeof(object), typeof(ChangePropertyAction), null);

        public string TargetName { get; set; }

        public string PropertyName { get; set; }

        public object Value
        {
            get { return GetValue(ValueProperty); }
            set { SetValue(ValueProperty, value); }
        }

        protected override void Invoke(object parameter)
        {
            var frameworkElement = AssociatedObject as FrameworkElement;
            if (frameworkElement == null || string.IsNullOrEmpty(TargetName) || string.IsNullOrEmpty(PropertyName)) return;

            var target = frameworkElement.FindName(TargetName) ?? frameworkElement;
            var property = target.GetType().GetProperty(PropertyName, BindingFlags.Public | BindingFlags.Instance);
            if (property == null || !property.CanWrite) return;

            var value = Value;
            if (value is string && property.PropertyType != typeof(string))
            {
                value = Convert.ChangeType(value, property.PropertyType, CultureInfo.InvariantCulture);
            }
            property.SetValue(target, value, null);
        }
    }
}

namespace Microsoft.Expression.Interactivity.Media
{
    public enum ControlStoryboardOption
    {
        Play,
        Stop
    }

    public class ControlStoryboardAction : TriggerAction<DependencyObject>
    {
        public static readonly DependencyProperty StoryboardProperty = DependencyProperty.Register("Storyboard", typeof(Storyboard), typeof(ControlStoryboardAction), null);

        public Storyboard Storyboard
        {
            get { return (Storyboard)GetValue(StoryboardProperty); }
            set { SetValue(StoryboardProperty, value); }
        }

        public ControlStoryboardOption ControlStoryboardOption { get; set; }

        protected override void Invoke(object parameter)
        {
            if (Storyboard == null) return;

            if (ControlStoryboardOption == ControlStoryboardOption.Stop)
            {
                Storyboard.Stop();
            }
            else
            {
                Storyboard.Begin();
            }
        }
    }
}
